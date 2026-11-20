#!/usr/bin/env python3
"""Ruft ein C6-Telefon an und spricht eine Nachricht, z. B. eine Terminerinnerung.

Beispiele:
  python erinnerung.py --liste
  python erinnerung.py --an Kueche --text "Morgen um zehn Uhr hast du den Termin beim Zahnarzt."
  python erinnerung.py --text "Test" --nur-erzeugen probe.wav        # nur anhören, nicht anrufen
  python erinnerung.py --an Kueche --wav ansage.wav

Voreinstellungen (Stimmprobe, Modellpfade, ...) stehen in erinnerung.json neben diesem Skript
oder in der Datei aus --config; Kommandozeilenwerte gehen vor. Siehe README.md.

Rückgabewert: 0 zugestellt, 3 nicht angenommen, 4 abgelehnt/besetzt, 5 nicht erreichbar,
              6 vor dem Ende aufgelegt, 2 Bedienfehler.
"""
from __future__ import annotations

import argparse
import json
import logging
import pathlib
import shutil
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import c6call  # noqa: E402
import tts     # noqa: E402

EXIT = {c6call.Result.DELIVERED: 0, c6call.Result.NO_ANSWER: 3, c6call.Result.REJECTED: 4,
        c6call.Result.UNREACHABLE: 5, c6call.Result.HUNG_UP: 6}


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--config", help="JSON mit Voreinstellungen (Standard: erinnerung.json neben dem Skript)")

    g = p.add_argument_group("Ziel")
    g.add_argument("--an", metavar="NAME", help="Gerätename des Telefons (mDNS-Suche)")
    g.add_argument("--ip", help="IP-Adresse statt Namenssuche")
    g.add_argument("--mac", help="MAC zur IP (aa:bb:cc:dd:ee:ff), für den statischen ARP-Eintrag")
    g.add_argument("--liste", action="store_true", help="Telefone im WLAN anzeigen und beenden")

    g = p.add_argument_group("Nachricht")
    g.add_argument("--text", help="Text, der gesprochen wird")
    g.add_argument("--wav", help="Fertige WAV-Datei statt Text")
    g.add_argument("--nur-erzeugen", metavar="WAV",
                   help="Nur die Telefon-Fassung (16 kHz) als WAV speichern, nicht anrufen")
    g.add_argument("--lautstaerke", type=float, default=0.7, help="Spitzenpegel 0..1 (Standard 0.7)")

    g = p.add_argument_group("Sprachausgabe")
    g.add_argument("--tts", choices=["f5", "befehl"], default="f5")
    g.add_argument("--ref-audio", help="Stimmprobe für F5-TTS (WAV, 5-15 s)")
    g.add_argument("--ref-text", default="", help="Was in der Stimmprobe gesagt wird (leer: Whisper)")
    g.add_argument("--f5-modell", default="F5TTS_v1_Base", help="F5-Architektur laut Modellkarte")
    g.add_argument("--f5-ckpt", default="", help="Checkpoint, Pfad oder hf://nutzer/repo/datei")
    g.add_argument("--f5-vocab", default="", help="vocab.txt, Pfad oder hf://...")
    g.add_argument("--tempo", type=float, default=1.0, help="Sprechtempo (F5), z. B. 0.9 = langsamer")
    g.add_argument("--tts-befehl", help="Eigenes TTS: Befehl mit {text} und {out}")

    g = p.add_argument_group("Anruf")
    g.add_argument("--anrufer-name", default="Erinnerung", help="Name auf dem Display (ohne Umlaute)")
    g.add_argument("--klingeln", type=float, default=40, help="Sekunden klingeln lassen")
    g.add_argument("--wiederholen", type=int, default=2, help="Nachricht so oft abspielen")
    g.add_argument("--versuche", type=int, default=1, help="Anrufversuche, falls niemand abnimmt")
    g.add_argument("--pause-min", type=float, default=5, help="Minuten zwischen den Versuchen")
    g.add_argument("--kein-arp", action="store_true", help="Keinen statischen ARP-Eintrag setzen")
    g.add_argument("--bind", default="0.0.0.0", help="Lokale Adresse für die UDP-Ports")
    p.add_argument("-v", "--verbose", action="store_true")
    return p


def load_config(p: argparse.ArgumentParser, argv) -> argparse.Namespace:
    pre, _ = p.parse_known_args(argv)
    path = pathlib.Path(pre.config) if pre.config else HERE / "erinnerung.json"
    if path.exists():
        cfg = json.loads(path.read_text(encoding="utf-8"))
        dests = {a.dest for a in p._actions}
        unknown = [k for k in cfg if k.replace("-", "_") not in dests]
        if unknown:
            p.error(f"{path}: unbekannte Schlüssel {unknown}")
        p.set_defaults(**{k.replace("-", "_"): v for k, v in cfg.items()})
        # relative Pfade in der Konfiguration gelten relativ zur Konfigurationsdatei
        a = p.parse_args(argv)
        for k in ("ref_audio", "wav"):
            v = cfg.get(k) or cfg.get(k.replace("_", "-"))
            if v and getattr(a, k) == v and not pathlib.Path(v).is_absolute():
                setattr(a, k, str(path.parent / v))
        return a
    if pre.config:
        p.error(f"Konfiguration {path} nicht gefunden")
    return p.parse_args(argv)


def speech_file(a) -> str:
    if a.wav:
        return a.wav
    if not a.text:
        raise SystemExit("--text oder --wav angeben")
    if a.tts == "befehl" or a.tts_befehl:
        if not a.tts_befehl:
            raise SystemExit("--tts befehl braucht --tts-befehl")
        return tts.command(a.text, a.tts_befehl)
    return tts.f5(a.text, a.ref_audio, a.ref_text, a.f5_modell, a.f5_ckpt, a.f5_vocab, a.tempo)


def save_wav(path: str, pcm16) -> None:
    import wave
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(c6call.AUDIO_RATE)
        w.writeframes(pcm16.tobytes())


def main(argv=None) -> int:
    p = parser()
    a = load_config(p, argv)
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(name)s: %(message)s", datefmt="%H:%M:%S")
    log = logging.getLogger("erinnerung")

    if a.liste:
        phones = c6call.discover()
        for ph in phones:
            print(f"{ph.name:<26} {ph.ip:<16} {ph.mac or '-'}")
        if not phones:
            print("Keine Telefone gefunden. Display an? Gleiches WLAN, kein Gastnetz?")
        return 0

    if not a.nur_erzeugen and not (a.an or a.ip):
        p.error("--an NAME oder --ip angeben (oder --nur-erzeugen)")

    src = speech_file(a)
    x, rate = c6call.load_wav(src)
    pcm = c6call.prepare_audio(x, rate, a.lautstaerke)
    log.info("Nachricht: %.1f s", len(pcm) / c6call.AUDIO_RATE)

    if a.nur_erzeugen:
        save_wav(a.nur_erzeugen, pcm)
        if src != a.wav:
            shutil.copyfile(src, str(pathlib.Path(a.nur_erzeugen).with_suffix(".original.wav")))
        log.info("gespeichert: %s (so klingt es am Telefon)", a.nur_erzeugen)
        return 0

    frames = c6call.encode_frames(pcm)
    opts = c6call.CallOptions(caller_name=a.anrufer_name, ring_timeout_s=a.klingeln,
                              repeats=a.wiederholen, bind_ip=a.bind)
    result = c6call.Result.UNREACHABLE
    for attempt in range(1, max(1, a.versuche) + 1):
        if attempt > 1:
            log.info("nächster Versuch in %.0f min", a.pause_min)
            time.sleep(a.pause_min * 60)
        try:
            phone = c6call.find_phone(a.an) if a.an and not a.ip else c6call.Phone(a.an or a.ip, a.ip, a.mac)
        except LookupError as e:
            log.warning("%s", e)
            result = c6call.Result.UNREACHABLE
            continue
        log.info("Versuch %d/%d: rufe %s (%s) an", attempt, max(1, a.versuche), phone.name, phone.ip)
        arp = c6call.StaticArp(phone.ip, phone.mac, enabled=not a.kein_arp)
        with arp, c6call.Caller(phone.ip, opts) as c:
            result = c.call(frames)
        log.info("Ergebnis: %s", result)
        if result in (c6call.Result.DELIVERED, c6call.Result.HUNG_UP):
            break
    return EXIT[result]


if __name__ == "__main__":
    sys.exit(main())
