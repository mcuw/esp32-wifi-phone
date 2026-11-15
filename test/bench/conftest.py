"""Bench tests: run against two phones on the Embedded AI Harness testbench.

Not run in CI (no boards there). See README.md, section "Bench-Test".
"""
import json
import os
import pathlib
import time
import urllib.parse
import urllib.request
import uuid

import pytest


def pytest_addoption(parser):
    g = parser.getgroup("bench")
    g.addoption("--wt-url", default=os.environ.get("TESTBENCH_URL"),
                help="Testbench base URL, e.g. http://<bench>:8080 (env TESTBENCH_URL)")
    g.addoption("--caller-slot", default="SLOT2")
    g.addoption("--callee-slot", default="SLOT1")
    g.addoption("--caller-mac", default=None,
                help="Expected STA MAC of the caller board, aa:bb:cc:dd:ee:ff (checked before flashing)")
    g.addoption("--callee-mac", default=None,
                help="Expected STA MAC of the callee board")
    g.addoption("--caller-bin", default="build-autodial/c6_phone.bin",
                help="Autodial app image for the caller ('' = already flashed)")
    g.addoption("--callee-bin", default="build/c6_phone.bin",
                help="Normal app image for the callee; also flashed back to the caller "
                     "afterwards ('' = leave both boards as they are)")
    g.addoption("--log-dir", default=str(pathlib.Path(__file__).parent / "logs"),
                help="Where the serial logs of both boards are saved (every run, pass or fail)")


class Bench:
    """Minimal client for the testbench HTTP API (stdlib only)."""

    def __init__(self, url):
        self.url = url.rstrip("/")

    def _req(self, method, path, body=None, headers=None, timeout=60):
        req = urllib.request.Request(self.url + path, data=body, method=method, headers=headers or {})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())

    def get(self, path, **params):
        q = ("?" + urllib.parse.urlencode(params)) if params else ""
        return self._req("GET", path + q)

    def post(self, path, payload, timeout=60):
        return self._req("POST", path, json.dumps(payload).encode(),
                         {"Content-Type": "application/json"}, timeout)

    def flash_app(self, slot, image, offset=0x10000):
        """Flash one app image (bootloader, partition table and NVS stay untouched)."""
        boundary = uuid.uuid4().hex
        with open(image, "rb") as f:
            data = f.read()
        parts = []
        for name, value in (("slot", slot), ("chip", "esp32c6"), ("baud", "921600")):
            parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode())
        parts.append((f'--{boundary}\r\nContent-Disposition: form-data; name="bin@0x{offset:x}"; '
                      f'filename="{os.path.basename(image)}"\r\nContent-Type: application/octet-stream\r\n\r\n').encode()
                     + data + b"\r\n")
        parts.append(f"--{boundary}--\r\n".encode())
        r = self._req("POST", "/api/flash", b"".join(parts),
                      {"Content-Type": f"multipart/form-data; boundary={boundary}"}, timeout=300)
        assert r.get("ok"), f"flash {slot} failed: {r.get('error')}"

    def reset(self, slot):
        """Reboot the board; returns its boot lines."""
        r = self.post("/api/serial/reset", {"slot": slot})
        out = r.get("output", [])
        return out if isinstance(out, list) else out.splitlines()

    def last_ts(self, slot):
        lines = self.get("/api/serial/output", slot=slot, lines=1).get("lines", [])
        return lines[-1]["ts"] if lines else 0.0

    def lines_since(self, slot, ts):
        return self.get("/api/serial/output", slot=slot, since=ts).get("lines", [])


@pytest.fixture(scope="session")
def bench(pytestconfig):
    url = pytestconfig.getoption("--wt-url")
    if not url:
        pytest.skip("no testbench: pass --wt-url or set TESTBENCH_URL")
    b = Bench(url)
    try:
        b.get("/api/info")
    except OSError as e:
        pytest.skip(f"testbench {url} not reachable: {e}")
    return b
