"""Tests ohne Hardware: python -m unittest discover tools/erinnerung -v

- ADPCM bitgenau gegen main/adpcm.c (über ctypes; übersprungen, wenn kein C-Compiler da ist)
- Umrechnung auf 16 kHz
- Anruf gegen ein simuliertes Telefon auf 127.0.0.2, das sich wie main/call.c verhält
"""
import ctypes
import math
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import c6call  # noqa: E402

ROOT = HERE.parent.parent
PHONE_IP = "127.0.0.2"


def speechlike(n, rate=16000):
    t = np.arange(n) / rate
    env = 0.5 + 0.5 * np.sin(2 * math.pi * 3 * t)
    return (env * (0.5 * np.sin(2 * math.pi * 220 * t) + 0.2 * np.sin(2 * math.pi * 1300 * t))).astype(np.float32)


class AdpcmTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = None
        cc = shutil.which(os.environ.get("CC", "cc"))
        if not cc:
            return
        cls.tmp = tempfile.mkdtemp()
        so = os.path.join(cls.tmp, "adpcm.so")
        subprocess.run([cc, "-shared", "-fPIC", "-O1", "-I", str(ROOT / "main"),
                        str(ROOT / "main" / "adpcm.c"), "-o", so], check=True)
        cls.lib = ctypes.CDLL(so)

    @classmethod
    def tearDownClass(cls):
        if cls.lib is not None:
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_bitexact_with_firmware(self):
        if self.lib is None:
            self.skipTest("kein C-Compiler")

        class St(ctypes.Structure):
            _fields_ = [("pred", ctypes.c_int), ("idx", ctypes.c_int)]

        rng = np.random.default_rng(1)
        x = np.concatenate([speechlike(16000) * 32767, rng.integers(-32768, 32767, 3200)]).astype(np.int16)
        x[100:140] = 32767                                  # Übersteuerung -> clamp-Pfad
        st, py = St(0, 0), c6call.Adpcm()
        for i in range(0, len(x) - 319, 320):
            blk = x[i:i + 320]
            out = (ctypes.c_uint8 * 160)()
            self.lib.adpcm_encode(ctypes.byref(st), blk.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)), 320, out)
            self.assertEqual(py.encode(blk.tolist()), bytes(out), f"Block {i // 320}")
            self.assertEqual((py.pred, py.idx), (st.pred, st.idx))

    def test_frames_decode_independently(self):
        x = (speechlike(8000) * 20000).astype(np.int16)
        frames = c6call.encode_frames(x)
        y = []
        for pred, idx, data in frames:                      # jeder Block nur mit seinem Kopf
            y += c6call.Adpcm(pred, idx).decode(data)
        y = np.array(y[:len(x)], dtype=np.float64)
        snr = 10 * math.log10(np.sum(x.astype(np.float64) ** 2) / np.sum((x - y) ** 2))
        self.assertGreater(snr, 25)


class AudioTest(unittest.TestCase):
    def test_resample_keeps_frequency_and_level(self):
        src = 22050
        t = np.arange(src) / src
        y = c6call.resample(np.sin(2 * math.pi * 1000 * t).astype(np.float32), src)
        self.assertAlmostEqual(len(y), 16000, delta=1)
        spec = np.abs(np.fft.rfft(y[1000:15000] * np.hanning(14000)))
        self.assertAlmostEqual(np.argmax(spec) * 16000 / 14000, 1000, delta=3)
        self.assertAlmostEqual(float(np.max(np.abs(y[1000:15000]))), 1.0, delta=0.03)

    def test_resample_removes_alias(self):
        src = 48000                                         # 11 kHz liegt über der neuen Nyquist (8 kHz)
        t = np.arange(src) / src
        y = c6call.resample(np.sin(2 * math.pi * 11000 * t).astype(np.float32), src)
        self.assertLess(float(np.sqrt(np.mean(y[1000:-1000] ** 2))), 0.02)

    def test_prepare_and_wav_roundtrip(self):
        import wave
        x = speechlike(22050, 22050)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "a.wav")
            with wave.open(p, "wb") as w:
                w.setnchannels(2)
                w.setsampwidth(2)
                w.setframerate(22050)
                s = (x * 20000).astype("<i2")
                w.writeframes(np.stack([s, s], 1).tobytes())
            y, rate = c6call.load_wav(p)
        self.assertEqual(rate, 22050)
        pcm = c6call.prepare_audio(y, rate, peak=0.7)
        self.assertEqual(pcm.dtype, np.int16)
        self.assertAlmostEqual(int(np.max(np.abs(pcm))), round(0.7 * 32767), delta=2)
        self.assertEqual(pcm[0], 0)


class FakePhone(threading.Thread):
    """Angerufener wie main/call.c: RINGING auf INVITE, ACCEPT/REJECT/BYE, Audio empfangen."""

    def __init__(self, mode="accept", answer_after=0.3, hangup_after=None):
        super().__init__(daemon=True)
        self.mode, self.answer_after, self.hangup_after = mode, answer_after, hangup_after
        self.sig = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.media = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sig.bind((PHONE_IP, c6call.SIG_PORT))
        self.media.bind((PHONE_IP, c6call.MEDIA_PORT))
        self.sig.settimeout(0.02)
        self.media.settimeout(0.0)
        self.rx_sig, self.frames, self.arrivals, self.stop = [], [], [], False
        self.caller_name = None

    def send(self, typ, call_id, to):
        self.sig.sendto(c6call.sig_pack(typ, call_id, "Kueche"), (to, c6call.SIG_PORT))

    def run(self):
        state, call_id, peer, t_inv, t_act = "idle", None, None, 0, 0
        while not self.stop:
            try:
                data, (src, _) = self.sig.recvfrom(256)
                m = c6call.sig_unpack(data)
                if m:
                    typ, cid = m
                    self.rx_sig.append(typ)
                    if typ == c6call.SIG_INVITE and state == "idle" and self.mode != "silent":
                        state, call_id, peer, t_inv = "incoming", cid, src, time.monotonic()
                        self.caller_name = struct.unpack(c6call.SIG_FMT, data)[3].rstrip(b"\0").decode()
                        self.send(c6call.SIG_RINGING, cid, src)
                    elif typ == c6call.SIG_INVITE and state == "incoming" and cid == call_id:
                        self.send(c6call.SIG_RINGING, cid, src)
                    elif typ == c6call.SIG_BYE and cid == call_id and state != "idle":
                        state = "idle"
            except socket.timeout:
                pass
            now = time.monotonic()
            if state == "incoming" and self.mode in ("accept", "reject") and now - t_inv > self.answer_after:
                if self.mode == "reject":
                    self.send(c6call.SIG_REJECT, call_id, peer)
                    state = "idle"
                else:
                    self.send(c6call.SIG_ACCEPT, call_id, peer)
                    state, t_act = "active", now
            if state == "active" and self.hangup_after and now - t_act > self.hangup_after:
                self.send(c6call.SIG_BYE, call_id, peer)
                state = "idle"
            while True:
                try:
                    data, (src, _) = self.media.recvfrom(512)
                except (BlockingIOError, socket.timeout):
                    break
                m = c6call.media_unpack(data)
                if m and src == peer:
                    self.frames.append(m)
                    self.arrivals.append(time.monotonic())
                    if state == "active":                   # Mikrofon-Audio zurück, wie cap_task
                        self.media.sendto(data, (peer, c6call.MEDIA_PORT))

    def close(self):
        self.stop = True
        self.join(2)
        self.sig.close()
        self.media.close()


class CallTest(unittest.TestCase):
    def setUp(self):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind((PHONE_IP, 0))
            s.close()
        except OSError:
            self.skipTest(f"{PHONE_IP} nicht verfügbar")
        self.opts = c6call.CallOptions(bind_ip="127.0.0.1", lead_in_s=0.2, gap_s=0.2, repeats=2,
                                       ring_timeout_s=1.0)
        self.msg = (speechlike(8000) * 20000).astype(np.int16)     # 0,5 s = 25 Blöcke
        self.frames = c6call.encode_frames(self.msg)

    def run_call(self, phone, **kw):
        for k, v in kw.items():
            setattr(self.opts, k, v)
        phone.start()
        try:
            with c6call.Caller(PHONE_IP, self.opts) as c:
                t0 = time.monotonic()
                r = c.call(self.frames)
                return r, time.monotonic() - t0, c
        finally:
            time.sleep(0.1)
            phone.close()

    def test_delivered(self):
        ph = FakePhone("accept", answer_after=0.3)
        r, dt, c = self.run_call(ph, caller_name="Erinnerung")
        self.assertEqual(r, c6call.Result.DELIVERED)
        self.assertEqual(ph.caller_name, "Erinnerung")
        self.assertEqual(ph.rx_sig[0], c6call.SIG_INVITE)
        self.assertEqual(ph.rx_sig[-1], c6call.SIG_BYE)
        n = 10 + 25 + 10 + 25 + 10                                  # Vorlauf, 2x Nachricht, Pause, Auslauf
        self.assertEqual(len(ph.frames), n)
        seqs = [f[0] for f in ph.frames]
        self.assertEqual([(s - seqs[0]) & 0xFFFF for s in seqs], list(range(n)))
        # Takt: 20 ms je Block (die ersten drei kommen sofort)
        span = ph.arrivals[-1] - ph.arrivals[3]
        self.assertAlmostEqual(span, (n - 4) * 0.02, delta=0.08)
        # Inhalt: die erste Wiedergabe dekodiert wieder zur Nachricht
        y = []
        for _seq, pred, idx, data in ph.frames[10:35]:
            y += c6call.Adpcm(pred, idx).decode(data)
        corr = np.corrcoef(self.msg.astype(float), np.array(y[:len(self.msg)], float))[0, 1]
        self.assertGreater(corr, 0.99)
        # Rückkanal: das "Mikrofon" des Telefons kam beim Anrufer an
        self.assertGreater(len(c.heard), 50 * c6call.AUDIO_FRAME)

    def test_no_answer_sends_bye(self):
        ph = FakePhone("ring")
        r, dt, _ = self.run_call(ph)
        self.assertEqual(r, c6call.Result.NO_ANSWER)
        self.assertIn(c6call.SIG_BYE, ph.rx_sig)
        self.assertLess(dt, 3)
        self.assertEqual(ph.frames, [])

    def test_rejected(self):
        r, _, _ = self.run_call(FakePhone("reject", answer_after=0.3))
        self.assertEqual(r, c6call.Result.REJECTED)

    def test_unreachable(self):
        old = c6call.INVITE_EVERY_S, c6call.INVITE_MAX_TRIES
        c6call.INVITE_EVERY_S, c6call.INVITE_MAX_TRIES = 0.05, 4
        try:
            ph = FakePhone("silent")
            r, _, _ = self.run_call(ph)
        finally:
            c6call.INVITE_EVERY_S, c6call.INVITE_MAX_TRIES = old
        self.assertEqual(r, c6call.Result.UNREACHABLE)
        self.assertEqual(ph.rx_sig, [c6call.SIG_INVITE] * 4)

    def test_hangup_before_end(self):
        ph = FakePhone("accept", answer_after=0.1, hangup_after=0.4)   # mitten in der ersten Wiedergabe
        r, _, _ = self.run_call(ph)
        self.assertEqual(r, c6call.Result.HUNG_UP)
        self.assertNotIn(c6call.SIG_BYE, ph.rx_sig)                   # kein BYE zurück, Telefon hat aufgelegt

    def test_hangup_after_first_pass_counts(self):
        ph = FakePhone("accept", answer_after=0.1, hangup_after=0.75)  # in der Pause nach Durchgang 1
        r, _, _ = self.run_call(ph)
        self.assertEqual(r, c6call.Result.DELIVERED)


if __name__ == "__main__":
    unittest.main()
