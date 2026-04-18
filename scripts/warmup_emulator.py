#!/usr/bin/env python3
"""Boot the emulator for each platform, navigate through the picker,
and wait for park data to arrive. Persists "last park" (and last
destination for the uber app) in SPI flash so subsequent installs
auto-load without needing picker navigation.

Used by CI to pre-warm emulators before integration tests and screenshots.

Usage:
    python3 scripts/warmup_emulator.py emery gabbro            # default: wdw
    python3 scripts/warmup_emulator.py --dest all emery        # uber app
"""

import argparse
import json
import os
import select
import signal
import subprocess
import sys
import time
from pathlib import Path

import websocket

REPO_ROOT = Path(__file__).resolve().parent.parent

ENV = dict(os.environ)
ENV.setdefault("SDL_VIDEODRIVER", "dummy")
local_bin = Path.home() / ".local" / "bin"
if str(local_bin) not in ENV.get("PATH", ""):
    ENV["PATH"] = f"{local_bin}:{ENV.get('PATH', '')}"


def press_select(platform: str) -> bool:
    try:
        info = json.loads(Path("/tmp/pb-emulator.json").read_text())
        port = info[platform][list(info[platform].keys())[0]]["pypkjs"]["port"]
        ws = websocket.WebSocket()
        ws.settimeout(5)
        ws.connect(f"ws://localhost:{port}/")
        ws.send_binary(bytes([0x0B, 0x08, 4]))
        time.sleep(0.15)
        ws.send_binary(bytes([0x0B, 0x08, 0]))
        ws.close()
        return True
    except Exception as e:
        print(f"  press failed: {e}", flush=True)
        return False


def _is_uber(dest: str) -> bool:
    try:
        cfg = json.loads((REPO_ROOT / "destinations" / f"{dest}.json").read_text())
        return bool(cfg.get("multiDestination"))
    except Exception:
        return False


def warmup_platform(platform: str, dest: str, timeout: float = 120) -> bool:
    print(f"\n=== warming up {platform} ({dest}) ===", flush=True)

    pbw = REPO_ROOT / "dist" / dest / "build" / f"{dest}.pbw"
    if not pbw.exists():
        print(f"  {pbw} not found — skip", flush=True)
        return False

    uber = _is_uber(dest)
    if uber:
        # Wipe SPI flash so dest picker shows (not auto-launched last dest)
        flash = Path.home() / ".pebble-sdk" / "4.9.148" / platform / "qemu_spi_flash.bin"
        try:
            flash.unlink()
        except Exception:
            pass

    cmd = ["pebble", "install", "--emulator", platform, "--logs", str(pbw)]
    proc = subprocess.Popen(
        cmd, cwd=REPO_ROOT / "dist" / dest, env=ENV,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0,
    )

    end = time.monotonic() + timeout
    install_at = None
    dest_data_seen = False
    first_select_done = False
    pending_park_select_at = None
    buf = b""

    try:
        while time.monotonic() < end:
            # Uber: fire first SELECT once dest data has rendered (or 15s fallback)
            # Per-dest: fire 3s after install-ok as before
            if not first_select_done and install_at is not None:
                ready_to_select = False
                if uber:
                    ready_to_select = dest_data_seen or (
                        time.monotonic() - install_at >= 15
                    )
                else:
                    ready_to_select = time.monotonic() - install_at >= 3
                if ready_to_select:
                    print("  pressing SELECT on picker...", flush=True)
                    press_select(platform)
                    first_select_done = True

            # Uber multi-park: after "populated N parks", press SELECT to pick first park
            if pending_park_select_at is not None and time.monotonic() >= pending_park_select_at:
                print("  pressing SELECT on park picker...", flush=True)
                press_select(platform)
                pending_park_select_at = None

            ready, _, _ = select.select([proc.stdout], [], [], 0.5)
            if ready:
                chunk = os.read(proc.stdout.fileno(), 4096)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    line_bytes, buf = buf.split(b"\n", 1)
                    line = line_bytes.decode("utf-8", errors="replace").rstrip()
                    print(f"  {line}", flush=True)
                    if "App install succeeded" in line:
                        install_at = time.monotonic()
                    elif uber and "dest_picker:" in line and " dests in " in line:
                        dest_data_seen = True
                    elif uber and "populated " in line and " parks from destination" in line:
                        try:
                            n = int(line.split("populated ")[1].split(" ")[0])
                        except Exception:
                            n = 0
                        if n > 1 and pending_park_select_at is None:
                            pending_park_select_at = time.monotonic() + 1.5
                    elif "recv " in line:
                        print(f"  {platform}/{dest} warm: park data received", flush=True)
                        return True
    finally:
        try:
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=2)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass

    print(f"  WARNING: {platform}/{dest} warm-up timed out", flush=True)
    return False


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dest", default="wdw",
                   help="Destination to warm up (default: wdw). Use 'all' for uber app.")
    p.add_argument("platforms", nargs="*", default=["emery", "gabbro"],
                   help="Platforms to warm up (default: emery gabbro)")
    args = p.parse_args()

    ok = True
    for plat in args.platforms:
        if not warmup_platform(plat, args.dest):
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
