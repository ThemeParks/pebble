#!/usr/bin/env python3
"""
soak_test.py — long-running stability test for the WDW Pebble app.

The script:
  1. Builds the app with PEBBLE_TEST_MODE=1 so the C side cycles parks
     every 2s.
  2. Installs that build on the running emulator.
  3. Streams logs and counts each "TEST: switch" event.
  4. Asserts no crashes (fxAbort, App fault, allocation failures).
  5. After the soak finishes, rebuilds + installs the clean (production)
     version so the emulator isn't left running test firmware.

Pre-requisite: the Pebble emulator must already be running. Start it once with:
    SDL_VIDEODRIVER=dummy pebble install --emulator emery dist/wdw/build/wdw.pbw

Usage:
    python3 scripts/soak_test.py                  # 100 switches (default)
    python3 scripts/soak_test.py --switches 500   # longer soak
    python3 scripts/soak_test.py --no-restore     # leave TEST_MODE build installed
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
DEST_NAME = "wdw"
DIST_DIR = REPO_ROOT / "dist" / DEST_NAME
PBW = DIST_DIR / "build" / f"{DEST_NAME}.pbw"

ENV = dict(os.environ)
ENV.setdefault("SDL_VIDEODRIVER", "dummy")
local_bin = Path.home() / ".local" / "bin"
if str(local_bin) not in ENV.get("PATH", ""):
    ENV["PATH"] = f"{local_bin}:{ENV.get('PATH', '')}"


# ── Build helpers ─────────────────────────────────────────────────────────

def run(cmd: list[str], env: dict, cwd: Path, label: str) -> int:
    """Run a subprocess synchronously, streaming output. Returns exit code."""
    print(f"  $ {' '.join(cmd)}")
    proc = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"  \033[31m{label} failed (exit {proc.returncode})\033[0m")
        if proc.stdout: print(proc.stdout[-2000:])
        if proc.stderr: print(proc.stderr[-2000:])
    return proc.returncode


def build_and_install(test_mode: bool) -> bool:
    """Regenerate dist/<dest>/ from src/ + destinations/<dest>.json, then
    build + install. TEST_MODE flag is passed via PEBBLE_TEST_MODE env."""
    env = dict(ENV)
    if test_mode:
        env["PEBBLE_TEST_MODE"] = "1"
        print("Regenerating + building with TEST_MODE=1 …")
    else:
        env.pop("PEBBLE_TEST_MODE", None)
        print("Regenerating + building clean (production) …")

    # Sync dist/<dest>/ to whatever's currently in src/ + destination JSON
    if run(["python3", "scripts/build_dest.py", DEST_NAME],
           env, REPO_ROOT, "build_dest") != 0:
        return False
    if run(["pebble", "clean"], env, DIST_DIR, "pebble clean") != 0:
        return False
    if run(["pebble", "build"], env, DIST_DIR, "pebble build") != 0:
        return False
    if run(["pebble", "install", "--emulator", "emery", str(PBW)],
           env, DIST_DIR, "pebble install") != 0:
        return False
    return True


# ── Log streaming ─────────────────────────────────────────────────────────

CRASH_PATTERNS = [
    r"fxAbort",
    r"memory full",
    r"Slot allocation",
    r"Chunk allocation",
    r"App fault!",
]


class LogStream:
    """Spawns `pebble logs` and streams output line-by-line."""

    def __init__(self, platform: str = "emery"):
        self.platform = platform
        self.proc: Optional[subprocess.Popen] = None
        self.lines: list[str] = []
        self.lock = threading.Lock()
        self.stop_evt = threading.Event()

    def start(self):
        cmd = ["pebble", "logs", "--emulator", self.platform]
        self.proc = subprocess.Popen(
            cmd, env=ENV, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1,
        )
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        if self.proc is None or self.proc.stdout is None:
            return
        for line in self.proc.stdout:
            with self.lock:
                self.lines.append(line.rstrip("\n"))
            if self.stop_evt.is_set():
                break

    def stop(self):
        self.stop_evt.set()
        if self.proc:
            try:
                self.proc.send_signal(signal.SIGTERM)
                self.proc.wait(timeout=3)
            except Exception:
                try: self.proc.kill()
                except Exception: pass

    def drain(self) -> list[str]:
        with self.lock:
            new = self.lines[:]
            self.lines = []
        return new


def detect_crash(lines: list[str]) -> Optional[str]:
    for line in lines:
        for pat in CRASH_PATTERNS:
            if re.search(pat, line):
                return line
    return None


# ── Soak runner ───────────────────────────────────────────────────────────

def soak(target_switches: int) -> bool:
    print(f"\nSoaking — target {target_switches} switches\n")

    log = LogStream()
    log.start()

    switches = 0
    started_at = time.monotonic()
    last_switch_at = time.monotonic()

    try:
        while True:
            time.sleep(0.5)
            new_lines = log.drain()

            crash = detect_crash(new_lines)
            if crash:
                elapsed = time.monotonic() - started_at
                print()
                print(f"\033[31m✖ CRASH after {switches} switches "
                      f"({elapsed:.0f}s)\033[0m")
                print(f"  {crash}")
                return False

            for line in new_lines:
                m = re.search(r"TEST: switch #(\d+)", line)
                if m:
                    switches = int(m.group(1))
                    last_switch_at = time.monotonic()
                    if switches % 10 == 0 or switches <= 5:
                        elapsed = time.monotonic() - started_at
                        print(f"  switch #{switches}  ({elapsed:.0f}s elapsed)")

            if switches >= target_switches:
                elapsed = time.monotonic() - started_at
                print()
                print(f"\033[32m✓ {switches} switches in {elapsed:.0f}s — "
                      f"NO CRASH\033[0m")
                return True

            # Watchdog: if we haven't seen a switch in 30s, the harness is broken
            if time.monotonic() - last_switch_at > 30:
                print(f"\033[33m⚠ No switch in 30s — TEST_MODE not active?"
                      f"\033[0m")
                return False

    except KeyboardInterrupt:
        elapsed = time.monotonic() - started_at
        print(f"\n\nInterrupted at {switches} switches ({elapsed:.0f}s)")
        return switches >= target_switches
    finally:
        log.stop()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--switches", type=int, default=100,
                   help="Target switch count (default: 100)")
    p.add_argument("--no-restore", action="store_true",
                   help="Don't reinstall the production build at the end")
    p.add_argument("--no-build", action="store_true",
                   help="Skip build+install, assume TEST_MODE is already on the emulator")
    args = p.parse_args()

    if not args.no_build:
        if not build_and_install(test_mode=True):
            print("\033[31mBuild/install failed\033[0m")
            return 1
        # Give emulator a moment to swap apps
        time.sleep(2)

    ok = soak(args.switches)

    if not args.no_restore:
        print()
        print("Restoring production build …")
        build_and_install(test_mode=False)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
