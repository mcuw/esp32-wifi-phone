"""Issue #16: firmware update from a release manifest, with rollback.

Setup: two phones on the bench. The DUT (--caller-slot) gets image A over
USB; the other phone (--callee-slot) only has to run the phone firmware so
that A can call it. Images come from test/bench/build_ota.sh:

  A v0.0.1  autodial + OTA test hook (installs without a tap)
  B v0.0.2  OTA test hook
  C v0.0.3  OTA test hook, never marks itself valid

The manifest and images are served from the bench's firmware repository
(/firmware/c6phone-otatest/...), the URL is compiled into the images.

Checked:
  1. A offers B but refuses to install it while its call is in progress,
     installs it after the call; B comes up and marks itself valid.
  2. B does not offer its own version (v0.0.2 = v0.0.2).
  3. B installs C; C never confirms; after the verify timeout the bootloader
     runs B again, which reports the rollback and does not offer C again.
  4. No unexpected error lines and no crash on the DUT.
Afterwards the DUT gets --restore-dir (normal firmware, boots ota_0).
"""
import datetime
import json
import pathlib
import re
import time
import urllib.parse
import urllib.request

import pytest

from test_call_sleeping import CRASH, ERROR, KNOWN_ERRORS, check_board

PROJECT = "c6phone-otatest"
A, B, C = "v0.0.1", "v0.0.2", "v0.0.3"

LOG = re.compile(r"^[IWE] \((\d+)\) ([\w.-]+): (.*)$")


class Dut:
    """Collects the DUT's serial log across reboots."""

    def __init__(self, bench, slot):
        self.bench, self.slot = bench, slot
        self.raw = []
        self.since = bench.last_ts(slot)
        self.pos = 0                         # wait_for() searches from here

    def poll(self):
        new = self.bench.lines_since(self.slot, self.since)
        if new:
            self.since = new[-1]["ts"]
            self.raw += new

    def reset(self):
        """Reboot; the reply carries the boot lines the recorder misses."""
        self.poll()
        self.raw += [{"ts": None, "text": t} for t in self.bench.reset(self.slot)]
        self.since = self.bench.last_ts(self.slot)

    def wait_for(self, pattern, timeout, what):
        """Waits for a line matching 'pattern' after the last match; returns its index."""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while True:
            self.poll()
            for i in range(self.pos, len(self.raw)):
                if rx.search(self.raw[i]["text"]):
                    self.pos = i + 1
                    return i
            if time.monotonic() > deadline:
                pytest.fail(f"timeout ({timeout} s) waiting for: {what} /{pattern}/")
            time.sleep(3)

    def texts(self, start=0, end=None):
        return [e["text"] for e in self.raw[start:end]]

    def save(self, log_dir):
        log_dir.mkdir(parents=True, exist_ok=True)
        path = log_dir / f"{datetime.datetime.now():%Y%m%d-%H%M%S}-ota-{self.slot}.log"
        with open(path, "w") as f:
            for e in self.raw:
                t = datetime.datetime.fromtimestamp(e["ts"]).strftime("%H:%M:%S.%f")[:-3] if e["ts"] else "(reset)"
                f.write(f"{t}  {e['text']}\n")
        print("serial log:", path)


def image(build_dir):
    p = pathlib.Path(build_dir) / "c6_phone.bin"
    if not p.exists():
        pytest.skip(f"{p} missing - run test/bench/build_ota.sh")
    return p.read_bytes()


def app_version(data):
    i = data.find(bytes.fromhex("3254cdab"))          # esp_app_desc_t magic
    return data[i + 16:i + 48].split(b"\0")[0].decode() if i >= 0 else None


def upload_checked(bench, name, data):
    """Uploads to the bench's firmware repository and reads the file back.
    The repository strips trailing whitespace (bench bug, seen 2026-11-16:
    0x0a and 0x20): an app image ending in such a byte then arrives short
    and fails its hash check. Bytes after the image's SHA-256 are ignored,
    so such an image gets padding instead."""
    if data[-1:].isspace():
        data += b"\xff" * 16
    bench.upload_firmware(PROJECT, name, data)
    with urllib.request.urlopen(f"{bench.url}/firmware/{PROJECT}/{name}", timeout=60) as r:
        served = r.read()
    assert served == data, f"bench serves {name} with {len(served)} bytes, uploaded {len(data)}"


def offer(bench, base, version, data):
    """Uploads the image and points the manifest at it."""
    name = f"c6_phone-{version}.bin"
    upload_checked(bench, name, data)
    manifest = {"version": version, "url": f"{base}/firmware/{PROJECT}/{name}"}
    upload_checked(bench, "ota.json", json.dumps(manifest).encode())


def test_ota_update_and_rollback(bench, pytestconfig):
    o = pytestconfig.getoption
    dut, peer = o("--caller-slot"), o("--callee-slot")
    imgs = {v: image(o(f"--ota-dir-{k}")) for k, v in (("a", A), ("b", B), ("c", C))}
    for v, data in imgs.items():
        assert app_version(data) == v, f"image for {v} carries version {app_version(data)}"

    # Address under which the phones reach the bench (its Wi-Fi side).
    port = urllib.parse.urlparse(bench.url).port or 80
    base = f"http://{bench.get('/api/info')['host_ip']}:{port}"

    check_board(bench, dut, o("--caller-mac"))
    check_board(bench, peer, o("--callee-mac"))
    offer(bench, base, B, imgs[B])

    d = Dut(bench, dut)
    restore = o("--restore-dir")
    try:
        bench.flash_build(dut, o("--ota-dir-a"))       # also bootloader (rollback) + ota_data_initial
        # The peer sleeps in iTWT by now and would not answer mDNS: wake it
        # (reboot) while A boots, so both search during their first 20 s.
        bench.reset(peer)

        # --- 1. A -> B, refused while the call rings ------------------------
        # (the bench's recorder misses the first ~2 s after a flash, so A's
        # "running" line may be gone - "update available" names A's version)
        dial = d.wait_for(r"autodial: round 1: dial", 90, "A calls the other phone")
        d.wait_for(r"autodial: round 1: update check during the call", 10, "A checks during the call")
        d.wait_for(rf"ota: update available: {A} -> {B}", 60, "A sees B")
        refused = d.wait_for(r"ota: update refused: call in progress", 30, "install refused during the call")
        hangup = d.wait_for(r"autodial: round 1: hangup", 60, "A hangs up")
        assert dial < refused < hangup, "refusal not inside the call"
        d.wait_for(rf"ota: install {B}", 60, "A installs B after the call")
        # The bench's recorder loses the last ~3 s before a reboot by the
        # firmware ("progress 100%", "written, rebooting") and the boot log;
        # "status:" repeats the boot state once Wi-Fi is up.
        d.wait_for(r"ota: progress 50%", 120, "download of B under way")
        d.wait_for(rf"ota: firmware {B} marked valid", 180, "B installed, booted and marked itself valid")
        d.wait_for(rf"ota: status: running {B} from ota_1, booted pending verify$", 30, "B ran from ota_1 on probation")

        # --- 2. same version is not offered ---------------------------------
        d.wait_for(rf"ota: up to date \({B}, offered {B}\)", 90, "B does not offer itself")

        # --- 3. B -> C, C never confirms -> rollback to B --------------------
        offer(bench, base, C, imgs[C])
        d.reset()
        d.wait_for(rf"ota: (status: )?running {B} from ota_1", 60, "B after reset")
        d.wait_for(rf"ota: install {C}", 90, "B installs C")
        d.wait_for(r"ota: progress 50%", 120, "download of C under way")
        d.wait_for(rf"ota: test build: firmware {C} never marks itself valid", 180, "C installed, booted, does not confirm")
        d.wait_for(rf"ota: status: running {C} from ota_0, booted pending verify$", 30, "C ran from ota_0 on probation")
        # ("did not come up - rolling back" is printed right before the reboot
        # and usually lost, see above)
        d.wait_for(rf"ota: status: running {B} from ota_1, rolled back from {C}$", 150,
                   "bootloader rolled back to B, which reports it")
        d.wait_for(rf"ota: offered {C} was rolled back before - ignored", 90, "C is not offered again")
    finally:
        d.poll()
        d.save(pathlib.Path(o("--log-dir")))
        if restore:
            bench.flash_build(dut, restore)

    # --- 4. error lines and crashes -------------------------------------------
    problems = [t for t in d.texts()
                if (ERROR.match(t) and not any(k.search(t) for k in KNOWN_ERRORS)) or CRASH.search(t)]
    assert not problems, "unexpected log lines:\n" + "\n".join(f"  {t}" for t in problems)
