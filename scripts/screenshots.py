#!/usr/bin/env python3
"""
screenshots.py — boot the app on each target platform, drive it through
the main screens, and save screenshots to screenshots/<platform>/.

What it captures (in order, per platform):
  01_picker       — park picker (initial)
  02_loading      — loading screen (brief, may be skipped if cached)
  03_park_waits   — park view, WAITS tab
  04_park_shows   — park view, SHOWS tab
  05_settings     — settings main menu
  06_sort_menu    — SORT BY sub-menu
  07_fave_rides   — FAVE RIDES list
  08_fave_shows   — FAVE SHOWS list
  09_error        — error state (simulated by requesting an invalid park)

Pre-requisite: the target emulator must be running. The script kills and
restarts each platform's emulator, so you don't need to start them yourself.

Usage:
    python3 scripts/screenshots.py                   # emery + gabbro
    python3 scripts/screenshots.py --platform emery
    python3 scripts/screenshots.py --output docs/shots
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Optional

import websocket

REPO_ROOT = Path(__file__).resolve().parent.parent
# Per-invocation destination context, populated by main() from --dest.
DEST_NAME = "wdw"
DIST_DIR = REPO_ROOT / "dist" / DEST_NAME
PBW = DIST_DIR / "build" / f"{DEST_NAME}.pbw"
# Set True when driving the uber app — adds the destination picker prelude.
UBER_MODE = False


def _set_dest(name: str) -> None:
    """Rebind the module-level dest context. Called from main()."""
    global DEST_NAME, DIST_DIR, PBW, UBER_MODE
    DEST_NAME = name
    DIST_DIR = REPO_ROOT / "dist" / DEST_NAME
    PBW = DIST_DIR / "build" / f"{DEST_NAME}.pbw"
    # Uber mode is anything that renders the destination picker on first
    # launch — determined by the destination JSON's multiDestination flag.
    try:
        import json as _json
        cfg = _json.loads((REPO_ROOT / "destinations" / f"{name}.json").read_text())
        UBER_MODE = bool(cfg.get("multiDestination"))
    except Exception:
        UBER_MODE = False

ENV = dict(os.environ)
ENV.setdefault("SDL_VIDEODRIVER", "dummy")
local_bin = Path.home() / ".local" / "bin"
if str(local_bin) not in ENV.get("PATH", ""):
    ENV["PATH"] = f"{local_bin}:{ENV.get('PATH', '')}"


# ── Button protocol (pypkjs WebSocket) ────────────────────────────────────

QEMU_BUTTON = {"back": 1, "up": 2, "select": 4, "down": 8}


def pypkjs_port(platform: str) -> Optional[int]:
    try:
        info = json.loads((Path(tempfile.gettempdir()) / "pb-emulator.json").read_text())
        return info[platform][list(info[platform].keys())[0]]["pypkjs"]["port"]
    except Exception:
        return None


class Buttons:
    def __init__(self, platform: str):
        self.platform = platform
        self.ws: Optional[websocket.WebSocket] = None

    def _connect(self):
        if self.ws and self.ws.connected:
            return
        port = pypkjs_port(self.platform)
        if port is None:
            raise RuntimeError(f"No pypkjs port for {self.platform}")
        self.ws = websocket.WebSocket()
        self.ws.connect(f"ws://localhost:{port}/")

    def press(self, button: str, settle: float = 1.0):
        self._connect()
        state = QEMU_BUTTON[button]
        self.ws.send_binary(bytes([0x0B, 0x08, state]))
        time.sleep(0.18)
        self.ws.send_binary(bytes([0x0B, 0x08, 0]))
        time.sleep(settle)

    def close(self):
        if self.ws:
            try: self.ws.close()
            except Exception: pass


# ── Emulator / build control ──────────────────────────────────────────────

def shell(cmd: list[str], cwd: Optional[Path] = None,
          check: bool = True, timeout: float = 180) -> tuple[int, str]:
    """Run a command, capture output. Returns (exitcode, combined output)."""
    print(f"  $ {' '.join(cmd)}")
    proc = subprocess.run(cmd, cwd=cwd, env=ENV,
                          capture_output=True, text=True, timeout=timeout)
    out = (proc.stdout or "") + (proc.stderr or "")
    if check and proc.returncode != 0:
        print(out[-1500:])
    return proc.returncode, out


def kill_emulator():
    """Tear down any running Pebble emulator cleanly."""
    try:
        subprocess.run(["pebble", "kill"], env=ENV, capture_output=True, timeout=10)
    except Exception:
        pass
    # Backup: kill QEMU + pypkjs explicitly in case `pebble kill` misses one
    for pattern in ("qemu-pebble", "pypkjs"):
        subprocess.run(["pkill", "-9", "-f", pattern], env=ENV,
                       capture_output=True, check=False)
    # Wipe the state file so the next install spawns fresh
    try:
        (Path(tempfile.gettempdir()) / "pb-emulator.json").unlink()
    except Exception:
        pass
    time.sleep(1.5)


def start_and_wait(platform: str, timeout: float = 90,
                   dest_picker_cb: Optional[callable] = None) -> bool:
    """Install the PBW with --logs and watch the combined stream until we
    see a `recv` line (park data parsed).

    Handles three startup paths:
      - Per-destination app, auto-loading last park → recv arrives on its own.
      - Per-destination app, cold picker → press SELECT after install to
        pick the first park.
      - Uber app, cold dest picker → press SELECT after install to pick the
        first destination. If that destination is single-park, recv arrives;
        if multi-park, press SELECT again after the park picker shows.

    Trigger for the first SELECT is "App install succeeded" + 3s, because
    the C-side "starting" log fires before the --logs stream connects and
    can be missed on cold-start.

    If dest_picker_cb is provided and UBER_MODE is true, it's called right
    after "App install succeeded" but before the SELECT press, giving the
    caller a window to screenshot the destination picker."""
    cmd = ["pebble", "install", "--emulator", platform, "--logs", str(PBW)]
    print(f"  $ {' '.join(cmd)}")
    proc = subprocess.Popen(
        cmd, cwd=DIST_DIR, env=ENV,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0,
    )
    end = time.monotonic() + timeout
    btn = Buttons(platform)
    picker_select_scheduled_at: Optional[float] = None
    picker_select_cancelled = False
    dest_picker_cb_called = False
    pending_dest_parks_press: Optional[float] = None
    install_seen_at: Optional[float] = None
    dest_data_seen = False
    buf = b""

    import select as _sel

    def _should_fire_picker_select() -> bool:
        """Decide whether to fire the auto-SELECT on the picker.

        For per-destination builds: 3s after install.
        For uber builds: wait until we see "dest_picker: N dests in M sections"
        (the log from dest_picker.c parse_dest_data). Falls back to 15s after
        install if the log never arrives — worst-case shows the placeholder."""
        if picker_select_scheduled_at is None or picker_select_cancelled:
            return False
        if UBER_MODE:
            return dest_data_seen or (
                install_seen_at is not None
                and time.monotonic() - install_seen_at >= 15
            )
        return time.monotonic() >= picker_select_scheduled_at

    try:
        while time.monotonic() < end:
            if _should_fire_picker_select():
                # Uber mode: let caller screenshot the dest picker first
                if UBER_MODE and dest_picker_cb and not dest_picker_cb_called:
                    try:
                        dest_picker_cb()
                    except Exception as e:
                        print(f"  dest_picker_cb error: {e}")
                    dest_picker_cb_called = True
                try:
                    print("  → pressing SELECT on picker")
                    btn.press("select")
                except Exception as e:
                    print(f"  ✖ SELECT failed: {e}")
                picker_select_cancelled = True

            if (pending_dest_parks_press is not None
                    and time.monotonic() >= pending_dest_parks_press):
                # Uber multi-park: press SELECT on the first park
                try:
                    btn.press("select")
                except Exception:
                    pass
                pending_dest_parks_press = None

            ready, _, _ = _sel.select([proc.stdout], [], [], 0.2)
            if ready:
                chunk = os.read(proc.stdout.fileno(), 4096)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    line_bytes, buf = buf.split(b"\n", 1)
                    line = line_bytes.decode("utf-8", errors="replace").rstrip()
                    if "App install succeeded" in line and picker_select_scheduled_at is None:
                        picker_select_scheduled_at = time.monotonic() + 3.0
                        install_seen_at = time.monotonic()
                    elif UBER_MODE and "dest_picker:" in line and " dests in " in line:
                        # Destination list parsed — safe to screenshot + SELECT
                        dest_data_seen = True
                    elif "recv " in line:
                        picker_select_cancelled = True
                        pending_dest_parks_press = None
                        time.sleep(1.0)
                        return True
                    elif UBER_MODE and "populated " in line and " parks from destination" in line:
                        # "populated N parks from destination" — multi-park
                        # means the park picker is now showing. Schedule a
                        # SELECT to pick the first park; if N=1 the app
                        # auto-loaded and recv will arrive shortly.
                        try:
                            n = int(line.split("populated ")[1].split(" ")[0])
                        except Exception:
                            n = 0
                        if n > 1 and pending_dest_parks_press is None:
                            pending_dest_parks_press = time.monotonic() + 1.5
    finally:
        try:
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=2)
        except Exception:
            try: proc.kill()
            except Exception: pass
        btn.close()
    return False


# ── Screenshot capture ────────────────────────────────────────────────────

def shot(platform: str, out_dir: Path, name: str):
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"{name}.png"
    # Brief settle so a just-pushed window has a chance to finish drawing
    # — gabbro is noticeably slower than emery.
    time.sleep(0.4)
    shell(["pebble", "screenshot", "--emulator", platform,
           "--no-open", str(path)], check=False)
    if path.exists():
        print(f"  📸 {path.relative_to(REPO_ROOT)}")


# ── Orchestration per platform ────────────────────────────────────────────

def capture_platform(platform: str, out_root: Path):
    print(f"\n=== {platform} ({DEST_NAME}) ===\n")
    shots = out_root / platform
    btn = Buttons(platform)

    # Fresh emulator, fresh install + wait for first park data.
    kill_emulator()
    # Also wipe the SPI flash so uber app forgets last destination (if any)
    # — otherwise it'd auto-launch the last destination and skip the
    # destination picker we want to capture.
    if UBER_MODE:
        flash = Path.home() / ".pebble-sdk" / "4.9.148" / platform / "qemu_spi_flash.bin"
        try:
            flash.unlink()
        except Exception:
            pass

    def _capture_dest_picker():
        """Take dest picker screenshots while the app is waiting for our SELECT."""
        # Pause briefly to let the picker render after the data arrives
        time.sleep(0.4)
        shot(platform, shots, "00_dest_picker")
        # Scroll down a few rows to show a region header
        try:
            b = Buttons(platform)
            for _ in range(8):
                b.press("down", settle=0.2)
            time.sleep(0.3)
            shot(platform, shots, "00a_dest_picker_scrolled")
            # Scroll back to top so the first SELECT picks the first row
            for _ in range(12):
                b.press("up", settle=0.1)
            time.sleep(0.3)
            b.close()
        except Exception as e:
            print(f"  dest picker scroll error: {e}")

    if not start_and_wait(platform,
                          dest_picker_cb=_capture_dest_picker if UBER_MODE else None):
        print(f"  ⚠ No park data within timeout — skipping {platform}")
        return

    # --- park view WAITS (just loaded, header expanded) ---
    shot(platform, shots, "03_park_waits")

    # --- park view WAITS, scrolled down (header collapsed) ---
    btn.press("down")    # jump to row ~6, triggers collapse
    shot(platform, shots, "03a_park_waits_scrolled")
    btn.press("up")      # restore to top
    btn.press("up")      # (nudge in case the jump left us at 1..5)

    # --- park view SHOWS (SELECT cycles page) ---
    btn.press("select")
    shot(platform, shots, "04_park_shows")

    # --- settings main (SELECT again from SHOWS enters settings) ---
    btn.press("select")
    shot(platform, shots, "05_settings")

    # --- sort sub-menu (first option is Sort) ---
    btn.press("select")
    shot(platform, shots, "06_sort_menu")

    # --- back to settings main, then into Fave Attractions ---
    btn.press("back")
    btn.press("down")           # cursor row 0 (Sort) → row 1 (Fave Attractions)
    btn.press("select")
    shot(platform, shots, "07_fave_rides")

    # --- back to settings main (cursor resets to row 0 = Sort), down twice
    # to land on Fave Shows (row 2), then select. ---
    btn.press("back")
    btn.press("down")           # → row 1 (Fave Attractions)
    btn.press("down")           # → row 2 (Fave Shows)
    btn.press("select")
    shot(platform, shots, "08_fave_shows")

    # --- back all the way out to picker ---
    btn.press("back")           # → settings main
    btn.press("back")           # → park view
    btn.press("back")           # → shows (from SELECT earlier — or park view waits)
    btn.press("back", settle=1.2)
    shot(platform, shots, "01_picker")

    btn.close()
    print(f"  ✓ {platform} — captured into {shots.relative_to(REPO_ROOT)}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--platform", action="append",
                   help="Platform(s) to capture. Default: emery + gabbro")
    p.add_argument("--dest", default="wdw",
                   help="Destination build to screenshot (default: wdw). "
                        "Use 'all' for the uber app.")
    p.add_argument("--output", type=Path, default=None,
                   help="Output directory (default: screenshots/<dest>/)")
    p.add_argument("--no-build", action="store_true",
                   help="Skip pebble build — assume a current build is in dist/<dest>/build")
    args = p.parse_args()

    _set_dest(args.dest)
    output = args.output or (REPO_ROOT / "screenshots" / args.dest)

    platforms = args.platform or ["emery", "gabbro"]

    if not args.no_build:
        print(f"Regenerating dist/{DEST_NAME} + building …")
        rc, _ = shell(["python3", "scripts/build_dest.py", DEST_NAME],
                      cwd=REPO_ROOT)
        if rc != 0:
            print("dist regeneration failed")
            return 1
        rc, _ = shell(["pebble", "build"], cwd=DIST_DIR)
        if rc != 0:
            print("build failed")
            return 1
    if not PBW.exists():
        print(f"no pbw at {PBW}")
        return 1

    for plat in platforms:
        try:
            capture_platform(plat, output)
        except Exception as e:
            print(f"  ✖ {plat} failed: {e}")

    kill_emulator()
    print(f"\nDone. Screenshots in {output}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
