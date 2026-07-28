#!/usr/bin/env python3
"""
Integration test suite for the Pebble app — runs the real app in the emulator,
drives it with button presses, and asserts on log output and screenshots.

These tests are the safety net pure JS unit tests can't provide: they catch
bugs that only manifest in the real XS runtime (memory leaks, Piu API misuse,
timing issues, etc.) and exercise the full data pipeline from PKJS through
the UI.

Usage:
  python3 scripts/integration_tests.py              # run all tests
  python3 scripts/integration_tests.py fav          # run tests matching 'fav'
  python3 scripts/integration_tests.py -v           # verbose log output

Each test is a function defined below. Tests inherit from TestCase and have
access to:
  - install()                      — fresh install
  - press(button)                  — press a button
  - screenshot(path)               — capture current screen
  - logs(duration=15)              — start log capture, returns accumulated
  - wait(seconds)                  — sleep
  - assert_log_contains(pattern)   — regex match against captured logs
  - assert_log_count(pattern, n)   — exact count of matches
  - assert_no_crash()              — no "fxAbort" or "memory full"
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

import websocket

REPO_ROOT = Path(__file__).resolve().parent.parent
# Set by main() from --dest. Default is WDW for backwards compatibility.
DEST_NAME = "wdw"
DEFAULT_PBW = REPO_ROOT / "dist" / DEST_NAME / "build" / f"{DEST_NAME}.pbw"
# Uber app requires an extra SELECT to traverse the destination picker.
UBER_MODE = False


def _set_dest(name: str) -> None:
    global DEST_NAME, DEFAULT_PBW, UBER_MODE
    DEST_NAME = name
    DEFAULT_PBW = REPO_ROOT / "dist" / DEST_NAME / "build" / f"{DEST_NAME}.pbw"
    try:
        cfg = json.loads((REPO_ROOT / "destinations" / f"{name}.json").read_text())
        UBER_MODE = bool(cfg.get("multiDestination"))
    except Exception:
        UBER_MODE = False

# Emulator state lives here. Wiping localstorage between tests gives us
# hermetic runs — otherwise favourites/lastPark/cache bleed across tests
# and masked a real unfavourite regression during development.
PEBBLE_EMU_STATE = {
    "emery": Path.home() / ".pebble-sdk" / "4.9.148" / "emery",
    "gabbro": Path.home() / ".pebble-sdk" / "4.9.148" / "gabbro",
}

ENV = dict(os.environ)
ENV.setdefault("SDL_VIDEODRIVER", "dummy")
local_bin = Path.home() / ".local" / "bin"
if str(local_bin) not in ENV.get("PATH", ""):
    ENV["PATH"] = f"{local_bin}:{ENV.get('PATH', '')}"


# ── Test infrastructure ────────────────────────────────────────────────

class TestFailure(Exception):
    pass


# ── Direct QEMU button protocol ──────────────────────────────────────
# Sends button presses directly to the QEMU TCP serial port, bypassing
# pebble-tool's subprocess-per-press model.  The QEMU packet format:
#   signature  (2 bytes, big-endian): 0xFEED
#   protocol   (2 bytes, big-endian): 0x0008 = Button
#   length     (2 bytes, big-endian): 1
#   state      (1 byte):  bitmask of Back(1), Up(2), Select(4), Down(8)
#   footer     (2 bytes, big-endian): 0xBEEF
#
# This eliminated the consistent press-drop that occurred when each
# press was a separate `pebble emu-button` subprocess invocation.

QEMU_BUTTON_MAP = {"back": 1, "up": 2, "select": 4, "down": 8}


def get_pypkjs_port(platform: str) -> Optional[int]:
    """Read the pypkjs WebSocket port from the emulator state file."""
    try:
        info_path = Path(tempfile.gettempdir()) / "pb-emulator.json"
        info = json.loads(info_path.read_text())
        return info[platform][list(info[platform].keys())[0]]["pypkjs"]["port"]
    except Exception:
        return None


def _ws_button_msg(state: int) -> bytes:
    """Build a pypkjs WebSocket message for a QemuButton press/release.

    Wire format matches pebble-tool's WebSocketRelayQemu:
      type byte  0x0b  (WebSocketRelayQemu)
      protocol   0x08  (QemuButton)
      state byte       (bitmask: Back=1, Up=2, Select=4, Down=8)
    """
    return bytes([0x0B, 0x08, state])


@dataclass
class TestContext:
    platform: str = "emery"
    verbose: bool = False
    log_proc: Optional[subprocess.Popen] = None
    log_master: Optional[int] = None
    log_output: bytearray = field(default_factory=bytearray)
    # Snapshot of everything captured between install and the first
    # test running — lets us assert on startup-only lines like the
    # bulk-hours prefetch even after per-test log resets.
    startup_log: str = ""
    # Resolved at instance creation (not class def time) so --dest switches work.
    pbw: Path = field(default_factory=lambda: DEFAULT_PBW)
    _ws: Optional[websocket.WebSocket] = None

    # ── Emulator control ─────────────────────────────────────────────

    def kill_emulator(self) -> None:
        # Kill pebble install processes first (they're the parents), then
        # the emulator binaries. Uses -f (full cmdline match) because
        # pypkjs runs as "python -m pypkjs" and won't match by name.
        for pattern in ("pebble install", "pebble logs", "pypkjs", "qemu-pebble"):
            subprocess.run(["pkill", "-9", "-f", pattern], env=ENV, check=False)
        time.sleep(2)

    def wipe_pkjs_cache(self) -> None:
        """Clear the PKJS phone-side cache so fresh park data is fetched.

        This does NOT touch the watch's SPI flash — watch-side localStorage
        (favourites, lastPark) lives inside the flash image and survives
        external file deletion. Tests must be designed to work with
        persistent state, just like the real app.
        """
        state_dir = PEBBLE_EMU_STATE.get(self.platform)
        if state_dir is None or not state_dir.exists():
            return
        ls_dir = state_dir / "localstorage"
        if ls_dir.exists():
            for f in ls_dir.iterdir():
                try:
                    f.unlink()
                except OSError:
                    pass

    def start_log_capture(self) -> None:
        """Install the app and start capturing logs via subprocess pipe.

        Expects the emulator to already be running. Start it before tests:
            SDL_VIDEODRIVER=dummy pebble install --emulator emery build/pebble.pbw
        """
        if self.log_proc and self.log_proc.poll() is None:
            self.log_output = bytearray()
            return
        self.stop_log_capture()
        time.sleep(1)
        cmd = ["pebble", "install", "--emulator", self.platform, str(self.pbw), "--logs"]
        self.log_proc = subprocess.Popen(
            cmd, env=ENV,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0,
        )
        self.log_master = self.log_proc.stdout.fileno()
        self.log_output = bytearray()

    def stop_log_capture(self) -> None:
        self._close_ws()
        if self.log_proc:
            try:
                self.log_proc.send_signal(signal.SIGTERM)
                self.log_proc.wait(timeout=3)
            except Exception:
                try: self.log_proc.kill()
                except Exception: pass
            self.log_proc = None
        self.log_master = None

    def drain_logs(self, for_seconds: float = 0.2) -> None:
        """Read whatever's currently available on the log pty."""
        import select as sel
        if self.log_master is None:
            time.sleep(for_seconds)
            return
        end = time.monotonic() + for_seconds
        while time.monotonic() < end:
            remaining = end - time.monotonic()
            r, _, _ = sel.select([self.log_master], [], [], min(0.1, remaining))
            if self.log_master in r:
                try:
                    chunk = os.read(self.log_master, 4096)
                    if not chunk:
                        break
                    self.log_output.extend(chunk)
                except OSError:
                    break
            else:
                break

    def log_text(self) -> str:
        text = self.log_output.decode("utf-8", errors="replace")
        return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text)

    # ── Actions ───────────────────────────────────────────────────────

    def _ensure_ws(self) -> websocket.WebSocket:
        """Lazy-connect a persistent WebSocket to pypkjs for button presses.

        Using one connection for all presses in a test is both faster and
        more reliable than spawning a subprocess per press — the latter
        silently drops ~1 in 4 clicks due to connection-lifecycle issues.
        """
        if self._ws is None or not self._ws.connected:
            port = get_pypkjs_port(self.platform)
            if port is None:
                raise TestFailure("cannot find pypkjs port in pb-emulator.json")
            self._ws = websocket.WebSocket()
            self._ws.connect(f"ws://localhost:{port}/")
        return self._ws

    def _close_ws(self) -> None:
        if self._ws is not None:
            try:
                self._ws.close()
            except Exception:
                pass
            self._ws = None

    def press(self, button: str, settle: float = 1.0) -> None:
        """Send a button click via the persistent WebSocket to pypkjs.

        Protocol: send press (state bitmask), wait 150ms, send release
        (state=0). Then drain logs for `settle` seconds so the watch has
        time to process the event before the next press arrives.
        """
        self._log(f"  press {button}")
        state = QEMU_BUTTON_MAP.get(button)
        if state is None:
            raise TestFailure(f"unknown button: {button}")
        # pypkjs drops websocket clients when emu-bt-connection toggles the
        # BT link (the BT-drop test), and websocket-client's .connected stays
        # True on a half-dead socket — the failure only surfaces on send.
        # Reconnect once before giving up.
        for attempt in range(2):
            ws = self._ensure_ws()
            try:
                ws.send_binary(_ws_button_msg(state))
                time.sleep(0.15)
                ws.send_binary(_ws_button_msg(0))
                break
            except OSError:
                self._close_ws()
                if attempt == 1:
                    raise
                self._log(f"  press socket died — reconnecting")
        # Let the watch process the event
        self.drain_logs(settle)

    def wait(self, seconds: float) -> None:
        self._log(f"  wait {seconds}s")
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            remaining = end - time.monotonic()
            self.drain_logs(min(0.2, remaining))

    def wait_for_data(self, timeout: float = 90) -> None:
        """Wait until a `recv N bytes` line appears — that's the C-side
        api.c log emitted when park data lands. On first run the app
        sits on the picker (no last-park persisted); press SELECT after
        install to pick the first park. We trigger on "App install
        succeeded" + 3s delay rather than "wdw starting" because the
        C-side log fires before the --logs stream connects on cold-start."""
        self._log(f"  wait_for_data (up to {timeout}s)")
        picker_pressed = False
        park_pressed = False
        install_seen_at: float | None = None
        multi_park_seen_at: float | None = None
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.drain_logs(2.0)
            text = self.log_text()
            if re.search(r"recv \d+ bytes", text):
                self._log(f"  data received")
                return
            if install_seen_at is None and "App install succeeded" in text:
                install_seen_at = time.monotonic()
            # Uber: trigger on dest-picker data load instead of fixed delay
            ready_for_first_select = False
            if UBER_MODE:
                if re.search(r"dest_picker: \d+ dests in \d+ sections", text):
                    ready_for_first_select = True
                elif install_seen_at is not None and time.monotonic() - install_seen_at >= 15:
                    ready_for_first_select = True  # fallback
            else:
                ready_for_first_select = (install_seen_at is not None
                                          and time.monotonic() - install_seen_at >= 3.0)
            if not picker_pressed and ready_for_first_select:
                self._log(f"  pressing select on picker")
                self.press("select", settle=2.0)
                picker_pressed = True
            # Uber multi-park: after "populated N parks from destination" where N>1,
            # press SELECT again to pick the first park.
            if UBER_MODE and not park_pressed:
                m = re.search(r"populated (\d+) parks from destination", text)
                if m and int(m.group(1)) > 1:
                    if multi_park_seen_at is None:
                        multi_park_seen_at = time.monotonic()
                    elif time.monotonic() - multi_park_seen_at >= 1.5:
                        self._log(f"  pressing select on park picker")
                        self.press("select", settle=2.0)
                        park_pressed = True
        raise TestFailure(f"no park data received within {timeout}s")

    def screenshot(self, path: str) -> None:
        out = Path(path) if Path(path).is_absolute() else REPO_ROOT / path
        out.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            ["pebble", "screenshot", "--emulator", self.platform, str(out)],
            env=ENV, capture_output=True, timeout=15, check=False,
        )

    def _log(self, msg: str) -> None:
        if self.verbose:
            print(msg, flush=True)

    # ── Assertions ────────────────────────────────────────────────────

    def assert_log_contains(self, pattern: str) -> None:
        self.drain_logs(0.2)
        text = self.log_text()
        if not re.search(pattern, text):
            raise TestFailure(f"expected log to contain /{pattern}/\n--- logs ---\n{text[-2000:]}")

    def assert_log_not_contains(self, pattern: str) -> None:
        self.drain_logs(0.2)
        text = self.log_text()
        if re.search(pattern, text):
            raise TestFailure(f"expected log NOT to contain /{pattern}/\n--- logs ---\n{text[-2000:]}")

    def assert_no_crash(self) -> None:
        self.drain_logs(0.3)
        text = self.log_text()
        patterns = [r"fxAbort", r"memory full", r"Slot allocation", r"Chunk allocation", r"App fault!"]
        for p in patterns:
            if re.search(p, text):
                raise TestFailure(f"crash detected: /{p}/\n--- logs ---\n{text[-2000:]}")

    def count_log(self, pattern: str) -> int:
        self.drain_logs(0.2)
        return len(re.findall(pattern, self.log_text()))


# ── Test registration ──────────────────────────────────────────────────

TESTS: list[tuple[str, Callable[[TestContext], None]]] = []


def test(name: str) -> Callable:
    def decorator(fn: Callable[[TestContext], None]) -> Callable:
        TESTS.append((name, fn))
        return fn
    return decorator


# ── Tests ──────────────────────────────────────────────────────────────

@test("app starts and loads park data")
def test_app_starts(ctx: TestContext) -> None:
    # api.c emits `recv N bytes` after a successful park-data parse,
    # park_view.c emits `PV top …` each time it re-sorts. Both happen
    # during the startup sequence so we inspect startup_log (current
    # buffer may already be empty after the per-test reset).
    blob = ctx.startup_log + ctx.log_text()
    if not re.search(r"recv \d+ bytes", blob):
        raise TestFailure("no 'recv N bytes' seen at startup")
    if not re.search(r"PV top ", blob):
        raise TestFailure("no 'PV top …' log — park view never rendered?")
    ctx.assert_no_crash()


@test("scrolling up and down does not crash")
def test_scroll_safety(ctx: TestContext) -> None:
    # PAGE_STEP is 6, so each press scrolls a chunk of rows.
    for _ in range(6):
        ctx.press("down")
        ctx.press("up")
    ctx.assert_no_crash()


@test("page switch Waits ↔ Shows stable")
def test_page_switch(ctx: TestContext) -> None:
    # Select cycles Waits → Shows → Settings; Back walks out. We stay
    # inside park+settings, cycling the pair 10 times.
    for _ in range(10):
        ctx.press("select")   # waits → shows
        ctx.press("select")   # shows → settings
        ctx.press("back")     # settings → shows
        ctx.press("back")     # shows → waits
    ctx.assert_no_crash()


@test("5 park reload cycles stable (memory)")
def test_refresh_cycles(ctx: TestContext) -> None:
    # Regression guard for the memory-crash saga that drove the C rewrite.
    # Back to picker, pick a park, wait for data. Repeat.
    for i in range(5):
        ctx.press("back", settle=2.0)     # waits → park picker
        ctx.press("select", settle=1.0)   # pick first park
        ctx.wait(8)                       # wait for data fetch
        ctx.assert_no_crash()
    ctx.assert_no_crash()


@test("favourite toggle: toggling ON pins to top")
def test_favourite_toggle(ctx: TestContext) -> None:
    # C-side logs:
    #   "SV toggle <id> ON|OFF" on each favourite toggle
    #   "PV top <name>[ FAV]"   each time the park list re-sorts
    #
    # The emulator occasionally drops button presses in long sequences,
    # so this test keeps it short: a single toggle of the first item in
    # the Fave Attractions list, then verify that IF the toggle landed
    # as ON, the park view's top row shows the FAV suffix. (If the
    # toggle landed as OFF, the first-item-might-not-be-top invariant
    # makes the assertion ambiguous; we only assert the ON path.)
    def open_fav_attractions():
        ctx.press("select", settle=2.0)      # waits → shows
        ctx.press("select", settle=2.5)      # shows → settings
        ctx.press("down", settle=1.5)        # cursor → "Fave Attractions"
        ctx.press("select", settle=2.0)      # enter list

    def back_to_waits():
        ctx.press("back", settle=1.5)        # fav list → settings
        ctx.press("back", settle=1.5)        # settings → shows
        ctx.press("back", settle=2.0)        # shows → waits

    # Perform one toggle then check. If it happened to land as OFF we
    # toggle once more (a fresh nav cycle) to force ON, then assert.
    # The emulator's button channel occasionally drops a press, which
    # derails the fixed sequence — retry the whole nav cycle once
    # before declaring failure.
    toggles: list = []
    for _attempt in range(2):
        open_fav_attractions()
        ctx.press("select", settle=1.2)      # single toggle
        back_to_waits()
        toggles = re.findall(r"SV toggle \S+ (ON|OFF)", ctx.log_text())
        if toggles:
            break
    if not toggles:
        raise TestFailure(
            "no SV toggle captured — button press lost or wrong screen?\n"
            f"--- log tail ---\n{ctx.log_text()[-1500:]}"
        )

    if toggles[-1] == "OFF":
        # Flip back to ON for the assertion below.
        open_fav_attractions()
        ctx.press("select", settle=1.2)
        back_to_waits()
        toggles = re.findall(r"SV toggle \S+ (ON|OFF)", ctx.log_text())
        if not toggles or toggles[-1] != "ON":
            raise TestFailure(
                "couldn't toggle back to ON — press lost?\n"
                f"  toggles: {toggles}"
            )

    # Last PV top line should show FAV on top now that we're in ON state.
    tops = re.findall(r"PV top .+", ctx.log_text())
    if not tops or " FAV" not in tops[-1]:
        raise TestFailure(
            "toggled ON but park view's top row isn't FAV\n"
            f"  toggles: {toggles}\n"
            f"  PV tops (last 5): {tops[-5:]}"
        )
    ctx.assert_no_crash()


@test("BT drop during fetch — app doesn't crash")
def test_bt_drop(ctx: TestContext) -> None:
    # We can't easily assert on the exact error-state render via logs
    # because pebble emu-bt-connection interacts oddly with pypkjs in
    # the emulator (the app sometimes exits cleanly via back-out rather
    # than surfacing the watchdog error line). What we DO care about
    # is that dropping the phone link doesn't crash the app —
    # fxAbort/App fault during or after a BT outage would be a
    # regression worth catching.
    def bt(state: str) -> None:
        subprocess.run(
            ["pebble", "emu-bt-connection", "--emulator", ctx.platform,
             "--connected", state],
            env=ENV, capture_output=True, timeout=8, check=False,
        )

    try:
        ctx.press("back", settle=1.5)        # waits → picker
        bt("no")
        time.sleep(0.5)
        ctx.press("select", settle=1.0)      # try to load — will fail
        ctx.wait(25)                         # past the 20s fetch watchdog
        ctx.assert_no_crash()
    finally:
        bt("yes")
        time.sleep(0.5)


@test("park hours prefetched at startup")
def test_hours_prefetch(ctx: TestContext) -> None:
    # api.c logs "cached hours for N park(s)" once the bulk hours reply
    # has been parsed. The request can lose its first outbox slot to
    # the initial park-data fetch (retried after 2s) so it may land
    # outside the startup_log snapshot — check both.
    combined = ctx.startup_log + ctx.log_text()
    if not re.search(r"cached hours for \d+ park\(s\)", combined):
        raise TestFailure(
            "expected 'cached hours for N park(s)' in logs\n"
            f"--- startup log ---\n{ctx.startup_log[-1500:]}\n"
            f"--- current log ---\n{ctx.log_text()[-1500:]}"
        )
    ctx.assert_no_crash()


# ── Uber-app-only tests ────────────────────────────────────────────────

# These only register when running with --dest=all. They assert on the
# extra startup steps the uber app takes: fetching the destination list,
# parsing it into the picker, and selecting a destination.

def _register_uber_tests():
    @test("uber: destination list fetched and parsed")
    def test_uber_dest_list(ctx: TestContext) -> None:
        # dest_picker.c logs "dest_picker: N dests in M sections" after
        # parsing a DEST_LIST_DATA payload from PKJS. We should have at
        # least the "Nearby" + one region section.
        blob = ctx.startup_log + ctx.log_text()
        m = re.search(r"dest_picker: (\d+) dests in (\d+) sections", blob)
        if not m:
            raise TestFailure(
                "no 'dest_picker: N dests in M sections' log — "
                "PKJS never sent DEST_LIST_DATA?\n"
                f"--- startup log ---\n{ctx.startup_log[-1500:]}"
            )
        dests, sections = int(m.group(1)), int(m.group(2))
        if dests < 10:
            raise TestFailure(f"only {dests} destinations parsed — expected many more")
        if sections < 1:
            raise TestFailure(f"only {sections} sections — expected Nearby + regions")

    @test("uber: destination selection populates parks")
    def test_uber_dest_select(ctx: TestContext) -> None:
        # main.c logs "populated N parks from destination" after
        # DEST_PARKS_DATA is parsed. This must fire at startup (from the
        # initial SELECT wait_for_data pressed on the dest picker).
        blob = ctx.startup_log + ctx.log_text()
        m = re.search(r"populated (\d+) parks from destination", blob)
        if not m:
            raise TestFailure(
                "no 'populated N parks' log — DEST_PARKS_DATA never arrived?"
            )
        n = int(m.group(1))
        if n < 1:
            raise TestFailure(f"populated 0 parks — destination empty?")

    @test("uber: destination name appears in select log")
    def test_uber_dest_select_log(ctx: TestContext) -> None:
        # dest_picker.c logs "dest_picker: selected <name>" on SELECT.
        blob = ctx.startup_log + ctx.log_text()
        m = re.search(r"dest_picker: selected (.+)", blob)
        if not m:
            raise TestFailure("no 'dest_picker: selected' log — SELECT lost?")

    @test("uber: BACK from single-park view returns to dest picker")
    def test_uber_back_single_park(ctx: TestContext) -> None:
        # Setup: we're in the park view of the destination selected at
        # startup — Nearby[0] in the dest picker. pypkjs fakes GPS by
        # GeoIP of the host's public IP, so which destination that is
        # depends on where the tests run (and only matters when it has
        # exactly 1 park; multi-park destinations skip below). BACK
        # should take us to the dest picker, not a spurious single-item
        # park picker.
        blob = ctx.startup_log + ctx.log_text()
        m = re.search(r"populated (\d+) parks from destination", blob)
        if not m or int(m.group(1)) != 1:
            # Selected a multi-park dest; skip this test by succeeding
            # silently — the sibling test_uber_back_multi_park covers it.
            return

        # Press BACK — should hide park_view and show dest_picker. If
        # instead the park_picker shows, a subsequent SELECT would log
        # "PV top <name>" (park_picker's SELECT loads the park and PV
        # re-renders). If dest_picker shows, SELECT logs
        # "dest_picker: selected <name>". We use that as the discriminator.
        ctx.press("back", settle=2.0)
        # Clear log so we only see events AFTER the BACK press
        ctx.log_output = bytearray()
        # The dest picker re-requests the (~7KB) destination list every
        # time it shows; wait for the fresh render marker instead of a
        # fixed delay so SELECT can't race the list transfer.
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            ctx.drain_logs(1.0)
            if re.search(r"dest_picker: \d+ dests in \d+ sections", ctx.log_text()):
                break
        ctx.press("select", settle=2.0)
        # Drain a bit more to catch async logs
        ctx.drain_logs(2.0)
        after = ctx.log_text()
        if re.search(r"dest_picker: selected", after):
            return  # correct — dest picker is showing
        raise TestFailure(
            "expected 'dest_picker: selected' after BACK+SELECT, meaning "
            "the dest picker is on top. Single-park destinations must skip "
            "the spurious park picker on BACK.\n"
            f"--- log after BACK+SELECT ---\n{after[-1500:]}"
        )

    @test("uber: BACK from multi-park view returns to park picker")
    def test_uber_back_multi_park(ctx: TestContext) -> None:
        # Only runs when the selected destination has multiple parks.
        blob = ctx.startup_log + ctx.log_text()
        m = re.search(r"populated (\d+) parks from destination", blob)
        if not m or int(m.group(1)) < 2:
            return  # single-park case handled by the sibling test

        # Press BACK. Park picker should show. SELECT on the first row
        # of the park picker loads that park, producing "recv N bytes"
        # followed by "PV top …". We assert on recv to avoid coupling
        # to specific park names.
        ctx.press("back", settle=2.0)
        ctx.log_output = bytearray()
        ctx.press("select", settle=2.0)
        ctx.drain_logs(3.0)
        after = ctx.log_text()
        if re.search(r"recv \d+ bytes", after):
            return  # correct — park picker selected a park
        raise TestFailure(
            "expected 'recv N bytes' after BACK+SELECT, meaning the park "
            "picker is on top and SELECT loaded a park.\n"
            f"--- log after BACK+SELECT ---\n{after[-1500:]}"
        )


# ── Runner ─────────────────────────────────────────────────────────────

def run_test(name: str, fn: Callable, ctx: TestContext) -> bool:
    start = time.monotonic()
    print(f"• {name}", end="", flush=True)
    try:
        fn(ctx)
        elapsed = time.monotonic() - start
        print(f" \033[32mok\033[0m ({elapsed:.1f}s)")
        return True
    except TestFailure as e:
        elapsed = time.monotonic() - start
        print(f" \033[31mFAIL\033[0m ({elapsed:.1f}s)")
        print(f"   {e}")
        return False
    except Exception as e:
        elapsed = time.monotonic() - start
        print(f" \033[31mERROR\033[0m ({elapsed:.1f}s)")
        print(f"   {type(e).__name__}: {e}")
        return False
    finally:
        pass  # don't stop — log process shared across tests


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("filter", nargs="?", default=None, help="only run tests whose name matches")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("--platform", choices=["emery", "gabbro"], default="emery")
    parser.add_argument("--dest", default="wdw",
                        help="Destination to test (default: wdw). Use 'all' for uber app.")
    args = parser.parse_args()

    _set_dest(args.dest)

    # Uber mode must start from the destination picker, not a persisted
    # last-destination — wipe SPI flash for the target platform.
    if UBER_MODE:
        flash = Path.home() / ".pebble-sdk" / "4.9.148" / args.platform / "qemu_spi_flash.bin"
        try:
            flash.unlink()
        except Exception:
            pass
        # Register uber-specific tests
        _register_uber_tests()

    if not DEFAULT_PBW.exists():
        print(f"error: {DEFAULT_PBW} missing —")
        print(f"       run `python3 scripts/build_dest.py {DEST_NAME} --build` first")
        return 1

    selected = TESTS
    if args.filter:
        selected = [(n, f) for n, f in TESTS if args.filter.lower() in n.lower()]

    if not selected:
        print("no tests matched")
        return 1

    # Create a single shared context — the emulator and log process persist
    # across all tests. The emulator must already be running:
    #   SDL_VIDEODRIVER=dummy pebble install --emulator emery build/pebble.pbw
    ctx = TestContext(platform=args.platform, verbose=args.verbose)
    ctx.start_log_capture()
    ctx.wait_for_data()
    # Give the hours-prefetch retry (api.c waits 2s if the outbox was
    # busy serving the park-data fetch) a chance to complete before we
    # snapshot. drain_logs breaks when the log goes quiet, so we sleep
    # first to ensure the retry has elapsed, then drain whatever arrived.
    time.sleep(4)
    ctx.drain_logs(1.0)
    ctx.startup_log = ctx.log_text()
    print(f"Running {len(selected)} test(s) on {args.platform}...\n")
    passed = 0
    failed = 0
    for name, fn in selected:
        # Reset log buffer between tests but keep the same log process
        ctx.log_output = bytearray()
        if run_test(name, fn, ctx):
            passed += 1
        else:
            failed += 1
    ctx.stop_log_capture()

    print()
    print(f"{passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
