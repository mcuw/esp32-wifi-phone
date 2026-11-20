"""Text -> WAV. Austauschbar: F5-TTS (Voice-Cloning, lokal auf der GPU) oder ein beliebiger Befehl.

Erzeugte Dateien werden zwischengespeichert, damit Wiederholungsversuche nicht neu rechnen.
"""
from __future__ import annotations

import hashlib
import json
import logging
import os
import pathlib
import shlex
import shutil
import subprocess
import tempfile

log = logging.getLogger("tts")

CACHE = pathlib.Path(os.environ.get("C6_TTS_CACHE",
                                    pathlib.Path.home() / ".cache" / "c6-erinnerung"))


def _cached(key: dict, make) -> str:
    h = hashlib.sha256(json.dumps(key, sort_keys=True).encode()).hexdigest()[:24]
    out = CACHE / f"{h}.wav"
    if out.exists() and out.stat().st_size > 44:
        log.info("Sprachdatei aus dem Zwischenspeicher: %s", out)
        return str(out)
    CACHE.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(suffix=".wav", dir=CACHE)
    os.close(fd)
    try:
        make(tmp)
        os.replace(tmp, out)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return str(out)


def _file_id(path: str) -> list:
    st = os.stat(path)
    return [os.path.abspath(path), st.st_size, int(st.st_mtime)]


def f5(text: str, ref_audio: str, ref_text: str = "", model: str = "F5TTS_v1_Base",
       ckpt: str = "", vocab: str = "", speed: float = 1.0, nfe_step: int = 32, seed: int | None = 7) -> str:
    """F5-TTS mit Stimmprobe. ref_text leer -> F5-TTS schreibt die Probe selbst mit Whisper ab.

    ckpt/vocab: lokale Pfade oder "hf://<nutzer>/<repo>/<datei>" (z. B. das deutsche Modell von aihpi).
    Ohne ckpt nimmt F5-TTS sein englisch/chinesisches Grundmodell - für Deutsch ungeeignet.
    """
    if not ref_audio or not os.path.exists(ref_audio):
        raise SystemExit(f"Stimmprobe fehlt: --ref-audio '{ref_audio}' (5-15 s WAV, eine Person, ohne Musik)")
    if not ckpt:
        log.warning("Kein --f5-ckpt angegeben: Das F5-Grundmodell spricht kein gutes Deutsch.")

    key = {"e": "f5", "text": text, "ref": _file_id(ref_audio), "ref_text": ref_text, "model": model,
           "ckpt": ckpt, "vocab": vocab, "speed": speed, "nfe": nfe_step, "seed": seed}

    def make(out: str):
        try:
            from f5_tts.api import F5TTS
        except ImportError as e:
            raise SystemExit("F5-TTS ist nicht installiert: siehe tools/erinnerung/README.md") from e
        from cached_path import cached_path            # kommt mit f5-tts; die API löst hf:// nicht selbst auf

        def resolve(p: str) -> str:
            return str(cached_path(p)) if p.startswith("hf://") else p

        log.info("F5-TTS rechnet ...")
        tts = F5TTS(model=model, ckpt_file=resolve(ckpt), vocab_file=resolve(vocab))
        tts.infer(ref_file=ref_audio, ref_text=ref_text, gen_text=text, speed=speed,
                  nfe_step=nfe_step, seed=seed, file_wave=out, show_info=log.debug)

    return _cached(key, make)


def command(text: str, template: str) -> str:
    """Beliebiges TTS-Programm. Platzhalter: {text} und {out} (Ziel-WAV), z. B.
    'python mein_tts.py --text {text} --out {out}'. Kein Shell-Aufruf, also kein Quoting nötig."""
    if "{out}" not in template:
        raise SystemExit("--tts-befehl braucht den Platzhalter {out}")
    parts = shlex.split(template, posix=os.name != "nt")
    key = {"e": "cmd", "text": text, "tpl": template}

    def make(out: str):
        argv = [p.replace("{text}", text).replace("{out}", out) for p in parts]
        exe = shutil.which(argv[0]) or argv[0]
        log.info("TTS-Befehl: %s", argv[0])
        r = subprocess.run([exe, *argv[1:]], capture_output=True, text=True)
        if r.returncode or not os.path.exists(out) or os.path.getsize(out) <= 44:
            raise SystemExit(f"TTS-Befehl fehlgeschlagen ({r.returncode}): {r.stderr.strip()[-500:]}")

    return _cached(key, make)
