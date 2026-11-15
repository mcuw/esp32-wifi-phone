"""Issue #5: a call must reach a phone that sleeps in iTWT.

Setup: two phones on the bench. The caller runs the autodial build
(sdkconfig.autodial), the callee the normal firmware. Both are rebooted, so
the caller's ARP cache is empty - the situation in which, before the fix, the
INVITE only got through by chance (callee's gratuitous ARP every 60 s).

Checked per call (round):
  * the callee was asleep (last iTWT event before the INVITE: "agreed")
  * the caller got the callee's MAC via mDNS and installed a static ARP entry
  * the very first INVITE already had an ARP entry ("try 0, arp yes")
  * RINGING arrived within RINGING_MAX_MS of dialling
  * after the caller hung up (ring time > IDLE_MS), the callee went back to
    iTWT within SLEEP_AGAIN_MAX_MS
"""
import re
import time

import pytest

RINGING_MAX_MS = 2000           # before the fix: up to 9.6 s, or never
SLEEP_AGAIN_MAX_MS = 25000      # IDLE_MS (20 s) + iTWT setup
RUN_TIMEOUT_S = 420             # autodial defaults: 45 + 2 x 30 + 50 s, plus boot

LOG = re.compile(r"^[IWE] \((\d+)\) ([\w.-]+): (.*)$")
MAC = re.compile(r"wifi:mode : sta \(([0-9a-f:]{17})\)")


def parse(lines):
    """[(ms_since_boot, tag, message)] from raw serial lines."""
    out = []
    for entry in lines:
        m = LOG.match(entry["text"] if isinstance(entry, dict) else entry)
        if m:
            out.append((int(m.group(1)), m.group(2), m.group(3)))
    return out


def board_mac(boot_lines):
    for line in boot_lines:
        m = MAC.search(line)
        if m:
            return m.group(1)
    return None


def check_board(bench, slot, expected):
    mac = board_mac(bench.reset(slot))
    assert mac, f"{slot}: no 'wifi:mode : sta (...)' in the boot log - is the phone firmware running?"
    if expected:
        assert mac == expected.lower(), f"{slot}: MAC {mac}, expected {expected} - wrong board in this slot?"
    return mac


@pytest.fixture
def boards(bench, pytestconfig):
    o = pytestconfig.getoption
    caller, callee = o("--caller-slot"), o("--callee-slot")
    # Verify the boards by MAC before anything is flashed.
    caller_mac = check_board(bench, caller, o("--caller-mac"))
    callee_mac = check_board(bench, callee, o("--callee-mac"))
    assert caller_mac != callee_mac

    flashed = False
    if o("--callee-bin"):
        bench.flash_app(callee, o("--callee-bin"))
    if o("--caller-bin"):
        bench.flash_app(caller, o("--caller-bin"))
        flashed = True
    yield caller, callee, callee_mac
    if flashed and o("--callee-bin"):
        bench.flash_app(caller, o("--callee-bin"))     # leave no autodial firmware behind


def collect(bench, caller, callee):
    """Reboot callee, then caller; record both logs until autodial is done."""
    since = {s: bench.last_ts(s) for s in (caller, callee)}
    raw = {caller: [], callee: []}
    bench.reset(callee)             # awake for IDLE_MS, so the caller finds it via mDNS
    bench.reset(caller)
    done_at = None
    deadline = time.monotonic() + RUN_TIMEOUT_S
    while time.monotonic() < deadline:
        time.sleep(5)
        for s in (caller, callee):
            new = bench.lines_since(s, since[s])
            if new:
                since[s] = new[-1]["ts"]
                raw[s] += new
        if done_at is None and any("autodial: done" in e["text"] for e in raw[caller]):
            done_at = time.monotonic()
        if done_at and time.monotonic() - done_at > SLEEP_AGAIN_MAX_MS / 1000 + 5:
            break
    # Only the boot we caused counts: cut at the last bootloader banner.
    for s in raw:
        starts = [i for i, e in enumerate(raw[s]) if "app_init: App version" in e["text"]]
        if starts:
            raw[s] = raw[s][starts[-1]:]
    return parse(raw[caller]), parse(raw[callee]), done_at is not None


def rounds_of(caller_log):
    """Split the caller log into one list of events per autodial round."""
    rounds, cur = [], None
    for ev in caller_log:
        t, tag, msg = ev
        if tag == "autodial" and re.match(r"round \d+: dial", msg):
            cur = [ev]
            rounds.append(cur)
        elif cur is not None:
            cur.append(ev)
    return rounds


def test_call_reaches_sleeping_phone(bench, boards):
    caller, callee, callee_mac = boards
    caller_log, callee_log, finished = collect(bench, caller, callee)

    assert any(tag == "autodial" and "peer" in msg for _, tag, msg in caller_log), \
        "caller never found the callee via mDNS (is the caller running the autodial build?)"
    assert finished, "autodial did not finish in time"

    rounds = rounds_of(caller_log)
    assert rounds, "no call was placed"

    # --- caller side -----------------------------------------------------
    for n, ev in enumerate(rounds, 1):
        t_dial, _, dial = ev[0]
        invites = [m for _, tag, m in ev if tag == "call" and m.startswith("sig tx INVITE")]
        arp_no = sum("arp NO" in m for m in invites)
        # The symptom first, then the diagnosis.
        ringing = [t for t, tag, m in ev if tag == "call" and m.startswith("sig rx RINGING")]
        assert ringing, f"round {n}: no RINGING - callee unreachable ({len(invites)} INVITEs, {arp_no} without ARP)"
        assert ringing[0] - t_dial <= RINGING_MAX_MS, \
            f"round {n}: RINGING after {ringing[0] - t_dial} ms (limit {RINGING_MAX_MS} ms, {arp_no} INVITEs without ARP)"
        assert dial.endswith("mac yes"), f"round {n}: peer without MAC in mDNS TXT: {dial}"
        arp = [m for _, tag, m in ev if tag == "call" and m.startswith("static ARP")]
        assert arp and arp[0].endswith(f"{callee_mac}: 0"), f"round {n}: static ARP not installed: {arp}"
        assert "arp yes" in invites[0], f"round {n}: first INVITE without ARP entry: {invites[0]}"

    # --- callee side -----------------------------------------------------
    itwt = None                     # last iTWT event: "agreed" / "torn down"
    invites, sleep_checks, bye_at = 0, [], None
    for t, tag, msg in callee_log:
        if tag == "net" and msg.startswith("iTWT agreed"):
            itwt = "agreed"
            if bye_at is not None:
                sleep_checks.append(t - bye_at)
                bye_at = None
        elif tag == "net" and msg.startswith("iTWT torn down"):
            itwt = "torn down"
        elif tag == "call" and msg.startswith("sig rx INVITE") and msg.endswith("(state 0)"):
            invites += 1
            assert itwt == "agreed", \
                f"call {invites}: callee was not asleep in iTWT (last iTWT event: {itwt}) - test not meaningful"
        elif tag == "call" and msg.startswith("sig rx BYE") and bye_at is None and invites > len(sleep_checks):
            bye_at = t
    assert invites == len(rounds), f"callee saw {invites} calls, caller placed {len(rounds)}"
    assert len(sleep_checks) == len(rounds), \
        f"callee did not return to iTWT after every call (only {len(sleep_checks)} of {len(rounds)})"
    for n, dt in enumerate(sleep_checks, 1):
        assert dt <= SLEEP_AGAIN_MAX_MS, f"call {n}: back in iTWT only {dt} ms after BYE"
