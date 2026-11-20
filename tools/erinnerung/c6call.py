"""Ein PC als Anrufer für die C6-Telefone: Anruf aufbauen und eine Audiodatei abspielen.

Gegenstück zu main/call.c, main/adpcm.c und main/peers.c (Werte aus main/app_config.h).
Nur stdlib + numpy; zeroconf wird erst für die Namenssuche per mDNS geladen.
"""
from __future__ import annotations

import dataclasses
import ipaddress
import logging
import platform
import random
import re
import select
import shutil
import socket
import struct
import subprocess
import time
import wave

import numpy as np

log = logging.getLogger("c6call")

# ---- main/app_config.h ---------------------------------------------------------------
SIG_PORT = 5070
MEDIA_PORT = 5072
SVC_TYPE = "_c6phone._udp.local."
NAME_MAX_LEN = 24
AUDIO_RATE = 16000
AUDIO_FRAME = 320                      # 20 ms

# ---- Paketformate (main/call.c, little endian, gepackt) -------------------------------
SIG_INVITE, SIG_RINGING, SIG_ACCEPT, SIG_REJECT, SIG_BYE = 1, 2, 3, 4, 5
SIG_NAMES = {1: "INVITE", 2: "RINGING", 3: "ACCEPT", 4: "REJECT", 5: "BYE"}
SIG_FMT = f"<BBH{NAME_MAX_LEN + 1}s"   # magic 0xC6, type, call_id, name[25]
MEDIA_HDR = "<BBHhBB"                  # magic 0xA1, flags, seq, pred, idx, rsv
SIG_SIZE = struct.calcsize(SIG_FMT)
MEDIA_SIZE = struct.calcsize(MEDIA_HDR) + AUDIO_FRAME // 2

INVITE_EVERY_S = 0.7                   # wie call.c: Angerufener kann ~1 s im iTWT schlafen
INVITE_MAX_TRIES = 12                  # ohne RINGING -> "nicht erreichbar"


def sig_pack(typ: int, call_id: int, name: str) -> bytes:
    raw = name.encode("utf-8")[:NAME_MAX_LEN]
    return struct.pack(SIG_FMT, 0xC6, typ, call_id, raw)


def sig_unpack(data: bytes):
    """-> (type, call_id) oder None."""
    if len(data) != SIG_SIZE:
        return None
    magic, typ, call_id, _ = struct.unpack(SIG_FMT, data)
    return (typ, call_id) if magic == 0xC6 else None


# ---- IMA-ADPCM, bitgenau wie main/adpcm.c ---------------------------------------------
_IDX = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
_STEP = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
    88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
    13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767)


class Adpcm:
    def __init__(self, pred: int = 0, idx: int = 0):
        self.pred, self.idx = pred, idx

    def _apply(self, code: int) -> None:
        step = _STEP[self.idx]
        vpdiff = step >> 3
        if code & 4:
            vpdiff += step
        if code & 2:
            vpdiff += step >> 1
        if code & 1:
            vpdiff += step >> 2
        p = self.pred - vpdiff if code & 8 else self.pred + vpdiff
        self.pred = 32767 if p > 32767 else (-32768 if p < -32768 else p)
        self.idx = min(88, max(0, self.idx + _IDX[code]))

    def _enc1(self, x: int) -> int:
        step = _STEP[self.idx]
        diff = x - self.pred
        code = 0
        if diff < 0:
            code, diff = 8, -diff
        if diff >= step:
            code |= 4
            diff -= step
        if diff >= step >> 1:
            code |= 2
            diff -= step >> 1
        if diff >= step >> 2:
            code |= 1
        self._apply(code)
        return code

    def encode(self, pcm) -> bytes:
        out = bytearray(len(pcm) // 2)
        for i in range(0, len(pcm) - 1, 2):
            lo = self._enc1(int(pcm[i]))
            hi = self._enc1(int(pcm[i + 1]))
            out[i // 2] = lo | (hi << 4)
        return bytes(out)

    def decode(self, data: bytes) -> list[int]:
        out = []
        for b in data:
            self._apply(b & 0x0F)
            out.append(self.pred)
            self._apply(b >> 4)
            out.append(self.pred)
        return out


def media_unpack(data: bytes):
    """-> (seq, pred, idx, adpcm_bytes) oder None."""
    if len(data) != MEDIA_SIZE:
        return None
    magic, _flags, seq, pred, idx, _ = struct.unpack_from(MEDIA_HDR, data)
    if magic != 0xA1:
        return None
    return seq, pred, idx, data[struct.calcsize(MEDIA_HDR):]


def encode_frames(pcm16: np.ndarray) -> list[tuple[int, int, bytes]]:
    """16-kHz-PCM -> Liste (pred, idx, daten) je 20-ms-Block, Zustand VOR dem Block im Kopf."""
    n = -(-len(pcm16) // AUDIO_FRAME) * AUDIO_FRAME
    pcm = np.zeros(n, dtype=np.int16)
    pcm[:len(pcm16)] = pcm16
    enc = Adpcm()
    frames = []
    for i in range(0, n, AUDIO_FRAME):
        pred, idx = enc.pred, enc.idx
        frames.append((pred, idx, enc.encode(pcm[i:i + AUDIO_FRAME].tolist())))
    return frames


# ---- Audio laden und auf 16 kHz bringen -----------------------------------------------
def load_wav(path: str) -> tuple[np.ndarray, int]:
    """WAV (8/16/24/32 bit PCM, mono/stereo) -> float32 mono in [-1, 1], Abtastrate."""
    with wave.open(path, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 1:
        x = (np.frombuffer(raw, np.uint8).astype(np.float32) - 128) / 128
    elif width == 2:
        x = np.frombuffer(raw, "<i2").astype(np.float32) / 32768
    elif width == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = (np.where(v >= 1 << 23, v - (1 << 24), v)).astype(np.float32) / (1 << 23)
    elif width == 4:
        x = np.frombuffer(raw, "<i4").astype(np.float32) / 2**31
    else:
        raise ValueError(f"{path}: {width * 8}-bit-WAV wird nicht unterstützt")
    return x.reshape(-1, ch).mean(axis=1), rate


def resample(x: np.ndarray, src: int, dst: int = AUDIO_RATE, taps: int = 32) -> np.ndarray:
    """Bandbegrenzte Umrechnung (gefensterter Sinc), reicht für Sprache."""
    if src == dst or len(x) == 0:
        return x.astype(np.float32)
    ratio = dst / src
    cutoff = min(1.0, ratio) * 0.92                       # Tiefpass knapp unter der neuen Nyquist
    n_out = int(len(x) * ratio)
    t = np.arange(n_out) / ratio                          # Position im Eingang
    base = np.floor(t).astype(np.int64)
    k = np.arange(-taps + 1, taps + 1)
    pos = base[:, None] + k[None, :]
    d = t[:, None] - pos
    w = cutoff * np.sinc(cutoff * d) * np.kaiser(2 * taps, 8.0)[None, :]
    xp = np.concatenate([np.zeros(taps), x, np.zeros(taps + 1)])
    out = np.empty(n_out, dtype=np.float32)
    step = 20000                                          # blockweise, damit der Speicher klein bleibt
    for i in range(0, n_out, step):
        out[i:i + step] = (xp[pos[i:i + step] + taps] * w[i:i + step]).sum(axis=1)
    return out


def prepare_audio(x: np.ndarray, rate: int, peak: float = 0.7) -> np.ndarray:
    """-> int16 mit 16 kHz: umrechnen, Gleichanteil weg, auf Spitzenpegel bringen, ein-/ausblenden."""
    y = resample(x, rate)
    y = y - float(np.mean(y)) if len(y) else y
    m = float(np.max(np.abs(y))) if len(y) else 0.0
    if m > 1e-6:
        y = y * (peak / m)
    fade = min(len(y) // 2, AUDIO_RATE // 50)              # 20 ms
    if fade:
        r = np.linspace(0, 1, fade, dtype=np.float32)
        y[:fade] *= r
        y[-fade:] *= r[::-1]
    return np.clip(np.round(y * 32767), -32768, 32767).astype(np.int16)


# ---- Telefone im WLAN finden (mDNS, wie main/peers.c) ----------------------------------
@dataclasses.dataclass
class Phone:
    name: str
    ip: str
    mac: str | None = None              # "aa:bb:cc:dd:ee:ff"


def discover(timeout: float = 3.0) -> list[Phone]:
    try:
        from zeroconf import ServiceBrowser, ServiceListener, Zeroconf
    except ImportError as e:
        raise SystemExit("Für die Suche per Name fehlt 'zeroconf' (pip install zeroconf) "
                         "- oder die Adresse mit --ip angeben.") from e

    found: dict[str, Phone] = {}

    class L(ServiceListener):
        def _add(self, zc, typ, name):
            info = zc.get_service_info(typ, name, timeout=2000)
            if not info:
                return
            ips = [a for a in info.parsed_addresses() if ":" not in a]
            if not ips:
                return
            inst = name[:-len(typ)].rstrip(".") if name.endswith(typ) else name
            mac = None
            raw = (info.properties or {}).get(b"mac")
            if raw and re.fullmatch(rb"[0-9a-fA-F]{12}", raw):
                h = raw.decode().lower()
                mac = ":".join(h[i:i + 2] for i in range(0, 12, 2))
            found[inst] = Phone(inst, ips[0], mac)

        def add_service(self, zc, typ, name):
            self._add(zc, typ, name)

        def update_service(self, zc, typ, name):
            self._add(zc, typ, name)

        def remove_service(self, zc, typ, name):
            pass

    zc = Zeroconf()
    try:
        ServiceBrowser(zc, SVC_TYPE, L())
        time.sleep(timeout)
    finally:
        zc.close()
    return sorted(found.values(), key=lambda p: p.name.lower())


def find_phone(name: str, timeout: float = 3.0) -> Phone:
    phones = discover(timeout)
    hit = [p for p in phones if p.name.lower() == name.lower()]
    if not hit:
        known = ", ".join(p.name for p in phones) or "keine"
        raise LookupError(f"Telefon '{name}' nicht gefunden (gefunden: {known}). Ist sein Display an? "
                          "Schlafende Telefone antworten auf mDNS unzuverlässig.")
    return hit[0]


# ---- Statischer ARP-Eintrag (Issue #5) -------------------------------------------------
class StaticArp:
    """Ein Telefon im iTWT-Schlaf beantwortet keine ARP-Anfragen. Wie call.c tragen wir seine
    MAC für die Dauer des Anrufs fest ein. Braucht Admin-/Root-Rechte; ohne geht es oft trotzdem
    (wenn das Telefon gerade wach ist oder der PC die MAC noch kennt), aber nicht verlässlich."""

    def __init__(self, ip: str, mac: str | None, enabled: bool = True):
        self.ip, self.mac, self.enabled, self.undo = ip, mac, enabled, None

    def __enter__(self):
        if not self.enabled:
            return self
        if not self.mac:
            log.warning("MAC des Telefons unbekannt (--mac): kein statischer ARP-Eintrag.")
            return self
        try:
            self.undo = self._set()
            log.info("statischer ARP-Eintrag %s -> %s", self.ip, self.mac)
        except Exception as e:                            # noqa: BLE001 - nur Hinweis, kein Abbruch
            log.warning("statischer ARP-Eintrag nicht möglich (%s). Als Admin/root starten, "
                        "sonst erreicht der Anruf ein schlafendes Telefon evtl. nicht.", e)
        return self

    def __exit__(self, *exc):
        if self.undo:
            try:
                subprocess.run(self.undo, check=False, capture_output=True, timeout=10)
            except Exception:                             # noqa: BLE001
                pass

    def _set(self):
        sysname = platform.system()
        if sysname == "Linux":
            r = subprocess.run(["ip", "route", "get", self.ip], capture_output=True, text=True, check=True)
            dev = re.search(r"\bdev (\S+)", r.stdout).group(1)
            subprocess.run(["ip", "neigh", "replace", self.ip, "lladdr", self.mac, "dev", dev,
                            "nud", "permanent"], capture_output=True, text=True, check=True)
            return ["ip", "neigh", "del", self.ip, "dev", dev]
        if sysname == "Windows":
            mac = self.mac.replace(":", "-")
            ps = shutil.which("powershell") or "powershell"
            idx = f"(Find-NetRoute -RemoteIPAddress {self.ip} | Select-Object -First 1).InterfaceIndex"
            cmd = (f"$i={idx}; Remove-NetNeighbor -InterfaceIndex $i -IPAddress {self.ip} "
                   f"-Confirm:$false -ErrorAction SilentlyContinue; "
                   f"New-NetNeighbor -InterfaceIndex $i -IPAddress {self.ip} -LinkLayerAddress {mac} "
                   f"-State Permanent -PolicyStore ActiveStore -ErrorAction Stop | Out-Null")
            r = subprocess.run([ps, "-NoProfile", "-Command", cmd], capture_output=True, text=True)
            if r.returncode:
                raise RuntimeError((r.stderr or r.stdout).strip().splitlines()[0] if (r.stderr or r.stdout)
                                   else f"PowerShell-Fehler {r.returncode}")
            return [ps, "-NoProfile", "-Command",
                    f"Remove-NetNeighbor -IPAddress {self.ip} -Confirm:$false -ErrorAction SilentlyContinue"]
        if sysname == "Darwin":
            subprocess.run(["arp", "-s", self.ip, self.mac], capture_output=True, text=True, check=True)
            return ["arp", "-d", self.ip]
        raise RuntimeError(f"nicht umgesetzt für {sysname}")


# ---- Der Anruf -------------------------------------------------------------------------
class Result:
    DELIVERED = "zugestellt"           # angenommen, Nachricht mindestens einmal ganz abgespielt
    HUNG_UP = "aufgelegt"              # angenommen, aber vor dem Ende der ersten Wiedergabe aufgelegt
    NO_ANSWER = "nicht angenommen"     # klingelte, niemand hat abgenommen
    REJECTED = "abgelehnt"             # abgelehnt oder besetzt
    UNREACHABLE = "nicht erreichbar"   # kein RINGING


@dataclasses.dataclass
class CallOptions:
    caller_name: str = "Erinnerung"    # erscheint auf dem Display (ohne Umlaute, Font kann sie nicht)
    ring_timeout_s: float = 40.0
    lead_in_s: float = 1.0             # Stille nach dem Abheben, bis das Telefon am Ohr ist
    repeats: int = 2
    gap_s: float = 1.5
    bind_ip: str = "0.0.0.0"
    sig_port: int = SIG_PORT           # Ports der Gegenstelle; nur für Tests änderbar
    media_port: int = MEDIA_PORT


class Caller:
    def __init__(self, target_ip: str, opts: CallOptions | None = None):
        self.ip = str(ipaddress.IPv4Address(target_ip))
        self.o = opts or CallOptions()
        self.call_id = random.randrange(1, 0x10000)
        self.sig = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.media = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Der Angerufene antwortet immer an Port 5070/5072 der Absenderadresse (call.c).
        try:
            self.sig.bind((self.o.bind_ip, SIG_PORT))
            self.media.bind((self.o.bind_ip, MEDIA_PORT))
        except OSError as e:
            self.close()
            raise SystemExit(f"UDP-Port {SIG_PORT}/{MEDIA_PORT} belegt ({e}). Läuft schon ein Anruf?") from e
        self.heard: list[int] = []     # vom Telefon empfangenes Audio (16 kHz), für spätere Auswertung

    def close(self):
        self.sig.close()
        self.media.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _send(self, typ: int):
        self.sig.sendto(sig_pack(typ, self.call_id, self.o.caller_name), (self.ip, self.o.sig_port))
        log.debug("sig tx %s", SIG_NAMES[typ])

    def _recv_sig(self, timeout: float):
        """Wartet bis zu timeout auf ein passendes Signalisierungspaket. -> Typ oder None."""
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return None
            r, _, _ = select.select([self.sig], [], [], left)
            if not r:
                return None
            data, (src, _port) = self.sig.recvfrom(256)
            m = sig_unpack(data)
            if not m or src != self.ip or m[1] != self.call_id:
                continue
            log.debug("sig rx %s", SIG_NAMES.get(m[0], m[0]))
            return m[0]

    def _drain_media(self):
        while select.select([self.media], [], [], 0)[0]:
            data, (src, _p) = self.media.recvfrom(512)
            m = media_unpack(data) if src == self.ip else None
            if m:
                self.heard.extend(Adpcm(m[1], m[2]).decode(m[3]))

    def ring(self) -> str | None:
        """INVITE senden, bis angenommen. -> None bei ACCEPT, sonst ein Result-Wert."""
        tries, rang, t_ring = 0, False, None
        while True:
            tries += 1
            if not rang and tries > INVITE_MAX_TRIES:
                return Result.UNREACHABLE
            self._send(SIG_INVITE)
            end = time.monotonic() + INVITE_EVERY_S
            while (left := end - time.monotonic()) > 0:
                t = self._recv_sig(left)
                if t == SIG_RINGING and not rang:
                    rang, t_ring = True, time.monotonic()
                    log.info("klingelt")
                elif t == SIG_ACCEPT:
                    log.info("angenommen")
                    return None
                elif t == SIG_REJECT:
                    return Result.REJECTED
                elif t == SIG_BYE:
                    return Result.REJECTED
            if rang and time.monotonic() - t_ring > self.o.ring_timeout_s:
                self._send(SIG_BYE)                       # beendet das Klingeln (call.c: BYE im Zustand INCOMING)
                return Result.NO_ANSWER

    def play(self, frames: list[tuple[int, int, bytes]]) -> str:
        """Nach ACCEPT: Nachricht abspielen, danach auflegen. -> Result."""
        seq = random.randrange(0x10000)
        silence = encode_frames(np.zeros(AUDIO_FRAME, np.int16))[0]
        lead = [silence] * round(self.o.lead_in_s * 50)
        gap = [silence] * round(self.o.gap_s * 50)
        plan: list[tuple[tuple[int, int, bytes], int]] = [(f, 0) for f in lead]   # (frame, durchgang)
        for r in range(1, max(1, self.o.repeats) + 1):
            if r > 1:
                plan += [(f, 0) for f in gap]
            plan += [(f, r) for f in frames]
        plan += [(silence, 0)] * 10                       # Jitter-Buffer beim Telefon leer laufen lassen

        done_passes = 0
        t0 = time.perf_counter()
        for i, ((pred, idx, data), pass_no) in enumerate(plan):
            # Takt 20 ms; die ersten 3 Blöcke sofort, damit der Jitter-Buffer (2 Blöcke) gefüllt ist.
            due = t0 + max(0, i - 3) * AUDIO_FRAME / AUDIO_RATE
            while True:
                t = self._recv_sig(max(0.0, due - time.perf_counter()))
                if t == SIG_BYE:
                    log.info("Telefon hat aufgelegt")
                    return Result.DELIVERED if done_passes >= 1 else Result.HUNG_UP
                if time.perf_counter() >= due:
                    break
            pkt = struct.pack(MEDIA_HDR, 0xA1, 0, seq & 0xFFFF, pred, idx, 0) + data
            self.media.sendto(pkt, (self.ip, self.o.media_port))
            seq += 1
            self._drain_media()
            nxt = plan[i + 1][1] if i + 1 < len(plan) else 0
            if pass_no and nxt != pass_no:
                done_passes = pass_no
        self._send(SIG_BYE)
        log.info("aufgelegt")
        return Result.DELIVERED

    def call(self, frames) -> str:
        r = self.ring()
        return r if r is not None else self.play(frames)
