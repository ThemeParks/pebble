#!/usr/bin/env python3
"""
Pebble emulator automation tool.

Wraps the `pebble` CLI with a Python-friendly interface for installing apps,
pressing buttons, taking screenshots, and capturing logs — all without needing
an SDL display.

Usage:
  scripts/emu.py install [--platform emery|gabbro] [--wipe] [--pbw PATH]
  scripts/emu.py press BUTTON [BUTTON ...] [--delay MS] [--hold MS]
  scripts/emu.py screenshot OUT_PATH
  scripts/emu.py logs [--duration SEC] [--out PATH]
  scripts/emu.py kill
  scripts/emu.py test [--wipe] [--platform emery|gabbro] [--duration SEC]
      Run a full flow: install, capture logs, print output.
  scripts/emu.py flow STEPS_JSON
      Run a scripted flow: JSON list of steps, each with 'action' and args.
      Actions: install, press, screenshot, sleep, logs.
  scripts/emu.py docshots
      Generate the canonical store-listing screenshots.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PBW = REPO_ROOT / "build" / "pebble.pbw"
DOCSHOT_DIR = REPO_ROOT / "docs" / "screenshots"

ENV = dict(os.environ)
ENV.setdefault("SDL_VIDEODRIVER", "dummy")
# Ensure pebble is on PATH even if the user's env doesn't include ~/.local/bin
local_bin = Path.home() / ".local" / "bin"
if str(local_bin) not in ENV.get("PATH", ""):
    ENV["PATH"] = f"{local_bin}:{ENV.get('PATH', '')}"


def run(cmd: list[str], check: bool = True, capture: bool = False, timeout: Optional[int] = None) -> subprocess.CompletedProcess:
    """Run a command and return the result."""
    return subprocess.run(
        cmd,
        env=ENV,
        check=check,
        capture_output=capture,
        text=True,
        timeout=timeout,
    )


def kill_emulator() -> None:
    """Forcefully kill any running Pebble emulator processes."""
    for name in ("qemu-pebble", "pypkjs"):
        subprocess.run(["pkill", "-9", "-f", name], env=ENV, check=False)
    time.sleep(1.5)


def wipe_emulator(platform: str) -> None:
    """Clear stored app state / localStorage for the given platform."""
    try:
        run(["pebble", "wipe"], capture=True, timeout=30)
    except Exception:
        pass


def install(platform: str, pbw: Path = DEFAULT_PBW, wipe: bool = False) -> None:
    """Install the given .pbw to the emulator. Auto-kills any existing instance."""
    kill_emulator()
    if wipe:
        wipe_emulator(platform)
    if not pbw.exists():
        raise FileNotFoundError(f"PBW not found: {pbw}")
    print(f"Installing {pbw.name} to {platform}...", flush=True)
    # Retry once on the first SDL/QEMU handshake failure
    for attempt in range(2):
        result = subprocess.run(
            ["pebble", "install", "--emulator", platform, str(pbw)],
            env=ENV, capture_output=True, text=True, timeout=60,
        )
        if result.returncode == 0 and "succeeded" in result.stdout:
            print(result.stdout.strip(), flush=True)
            return
        if attempt == 0:
            time.sleep(2)
            continue
        print(result.stdout, file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        raise RuntimeError(f"Install failed after retry")


def press(platform: str, buttons: list[str], delay_ms: int = 200, hold_ms: int = 100) -> None:
    """Press a sequence of buttons with a delay between each. Each `hold_ms` is click duration."""
    for btn in buttons:
        print(f"  press: {btn}", flush=True)
        result = subprocess.run(
            ["pebble", "emu-button", "--emulator", platform, "click", btn, "--duration", str(hold_ms)],
            env=ENV, capture_output=True, text=True, timeout=10,
        )
        if result.returncode != 0:
            print(f"  WARNING: button press failed for {btn}: {result.stderr}", file=sys.stderr)
        time.sleep(delay_ms / 1000.0)


def screenshot(platform: str, out_path: Path) -> None:
    """Capture a screenshot of the current emulator display."""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        ["pebble", "screenshot", "--emulator", platform, str(out_path)],
        env=ENV, capture_output=True, text=True, timeout=15,
    )
    if result.returncode != 0:
        raise RuntimeError(f"Screenshot failed: {result.stderr}")
    try:
        display = out_path.relative_to(REPO_ROOT)
    except ValueError:
        display = out_path
    print(f"  → {display}", flush=True)


def capture_logs(platform: str, duration_sec: int, pbw: Optional[Path] = None) -> str:
    """Install with --logs and stream for the given duration, returning captured output.

    The --logs flag is only reliable when paired with install (otherwise early
    startup logs aren't captured). If `pbw` is provided, we install first.
    """
    import pty
    import select
    import re

    cmd = ["pebble", "install", "--emulator", platform]
    if pbw:
        cmd.append(str(pbw))
    cmd.extend(["--logs"])

    # Use a pty so `pebble install --logs` doesn't buffer on non-tty
    master, slave = pty.openpty()
    proc = subprocess.Popen(cmd, env=ENV, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)

    output = bytearray()
    deadline = time.monotonic() + duration_sec
    try:
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            r, _, _ = select.select([master], [], [], min(0.5, remaining))
            if master in r:
                try:
                    chunk = os.read(master, 4096)
                    if not chunk:
                        break
                    output.extend(chunk)
                except OSError:
                    break
            if proc.poll() is not None:
                break
    finally:
        try:
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=3)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        os.close(master)

    text = output.decode("utf-8", errors="replace")
    # Strip ANSI escape codes
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text)
    return text


def run_flow(platform: str, steps: list[dict]) -> None:
    """Execute a scripted flow of actions."""
    for i, step in enumerate(steps):
        action = step["action"]
        print(f"[{i+1}/{len(steps)}] {action}", flush=True)
        if action == "install":
            pbw = Path(step.get("pbw", DEFAULT_PBW))
            install(platform, pbw=pbw, wipe=step.get("wipe", False))
        elif action == "press":
            buttons = step["buttons"]
            if isinstance(buttons, str):
                buttons = [buttons]
            press(platform, buttons, delay_ms=step.get("delay_ms", 200), hold_ms=step.get("hold_ms", 100))
        elif action == "screenshot":
            out = Path(step["out"])
            if not out.is_absolute():
                out = REPO_ROOT / out
            screenshot(platform, out)
        elif action == "sleep":
            seconds = step.get("seconds", 1)
            print(f"  sleep {seconds}s", flush=True)
            time.sleep(seconds)
        elif action == "logs":
            duration = step.get("duration", 10)
            text = capture_logs(platform, duration)
            print(text)
        else:
            raise ValueError(f"Unknown action: {action}")


def docshots(platform: str = "emery") -> None:
    """Capture store-listing and doc screenshots on the given platform.

    Flow:
      1. Fresh install + park picker
      2. Attractions page (scrolled top)
      3. Attractions page (scrolled down)
      4. Shows page
      5. Settings menu
      6. Sort sub-menu

    Fresh install each run so the state is reproducible.
    """
    DOCSHOT_DIR.mkdir(parents=True, exist_ok=True)

    steps = [
        {"action": "install", "wipe": True},
        {"action": "sleep", "seconds": 10},
        {"action": "screenshot", "out": str(DOCSHOT_DIR / f"01-park-picker-{platform}.png")},
        {"action": "press", "buttons": "select"},        # choose Magic Kingdom
        {"action": "sleep", "seconds": 12},              # wait for fetch
        {"action": "screenshot", "out": str(DOCSHOT_DIR / f"02-attractions-{platform}.png")},
        {"action": "press", "buttons": "down"},          # half-page scroll
        {"action": "sleep", "seconds": 2},
        {"action": "screenshot", "out": str(DOCSHOT_DIR / f"03-attractions-scrolled-{platform}.png")},
        {"action": "press", "buttons": "back"},          # back button to return to top (cycles through scroll)
        {"action": "sleep", "seconds": 2},
    ]
    # Re-install to reset to the attractions page for the remaining shots
    steps.extend([
        {"action": "install"},
        {"action": "sleep", "seconds": 12},
        {"action": "press", "buttons": "select"},        # waits → shows
        {"action": "sleep", "seconds": 3},
        {"action": "screenshot", "out": str(DOCSHOT_DIR / f"04-shows-{platform}.png")},
        {"action": "press", "buttons": "select"},        # shows → settings
        {"action": "sleep", "seconds": 3},
        {"action": "screenshot", "out": str(DOCSHOT_DIR / f"05-settings-{platform}.png")},
        {"action": "press", "buttons": "select"},        # settings → sort submenu
        {"action": "sleep", "seconds": 2},
        {"action": "screenshot", "out": str(DOCSHOT_DIR / f"06-sort-submenu-{platform}.png")},
    ])
    run_flow(platform, steps)
    print(f"\nDocshots saved to {DOCSHOT_DIR.relative_to(REPO_ROOT)}")


def cmd_install(args):
    install(args.platform, pbw=Path(args.pbw) if args.pbw else DEFAULT_PBW, wipe=args.wipe)


def cmd_press(args):
    press(args.platform, args.buttons, delay_ms=args.delay, hold_ms=args.hold)


def cmd_screenshot(args):
    screenshot(args.platform, Path(args.out))


def cmd_logs(args):
    pbw = Path(args.pbw) if args.pbw else None
    text = capture_logs(args.platform, args.duration, pbw=pbw)
    if args.out:
        Path(args.out).write_text(text)
        print(f"Wrote logs to {args.out}")
    else:
        print(text)


def cmd_kill(args):
    kill_emulator()
    print("killed")


def cmd_test(args):
    """Full flow: install, capture logs, print output."""
    text = capture_logs(args.platform, args.duration, pbw=DEFAULT_PBW)
    # Focus on interesting lines (case-insensitive match)
    keywords = ("xshost", "pkjs>", "error", "fault", "fxabort", "slot", "chunk", "main:", "sv:", "pv:")
    interesting = []
    for line in text.splitlines():
        low = line.lower()
        if any(k in low for k in keywords):
            interesting.append(line)
    print("\n".join(interesting) if interesting else text)


def cmd_flow(args):
    steps = json.loads(Path(args.steps_json).read_text())
    run_flow(args.platform, steps)


def cmd_trace(args):
    """Install with --logs, press buttons at scheduled times, print captured logs.

    Use `--press "sec:button"` to schedule a button press at `sec` seconds after
    install. Can be repeated. Example:
      emu.py trace --duration 30 --press 10:select --press 12:select --press 14:select
    """
    import pty
    import select as sel
    import re

    kill_emulator()
    time.sleep(2)

    presses = []
    for spec in args.press or []:
        at, btn = spec.split(":")
        presses.append((float(at), btn))
    presses.sort()

    cmd = ["pebble", "install", "--emulator", args.platform, str(DEFAULT_PBW), "--logs"]
    master, slave = pty.openpty()
    proc = subprocess.Popen(cmd, env=ENV, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)

    output = bytearray()
    deadline = time.monotonic() + args.duration
    start = time.monotonic()
    pending = list(presses)
    try:
        while time.monotonic() < deadline:
            elapsed = time.monotonic() - start
            # Fire any due button presses
            while pending and pending[0][0] <= elapsed:
                at, btn = pending.pop(0)
                print(f"  [t+{elapsed:.1f}s] press: {btn}", flush=True)
                subprocess.run(
                    ["pebble", "emu-button", "--emulator", args.platform, "click", btn],
                    env=ENV, capture_output=True, timeout=10, check=False,
                )
            # Read any available log output
            r, _, _ = sel.select([master], [], [], 0.2)
            if master in r:
                try:
                    chunk = os.read(master, 4096)
                    if not chunk:
                        break
                    output.extend(chunk)
                except OSError:
                    break
            if proc.poll() is not None:
                break
    finally:
        try:
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=3)
        except Exception:
            try: proc.kill()
            except: pass
        os.close(master)

    text = output.decode("utf-8", errors="replace")
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text)
    keywords = ("xshost", "pkjs>", "error", "fault", "fxabort", "slot", "chunk", "main:", "sv:", "pv:")
    for line in text.splitlines():
        if any(k in line.lower() for k in keywords):
            print(line)


def cmd_docshots(args):
    docshots(args.platform)


def main():
    parser = argparse.ArgumentParser(description="Pebble emulator automation")
    parser.add_argument("--platform", choices=["emery", "gabbro"], default="emery")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("install")
    p.add_argument("--wipe", action="store_true")
    p.add_argument("--pbw", type=str)
    p.set_defaults(func=cmd_install)

    p = sub.add_parser("press")
    p.add_argument("buttons", nargs="+", choices=["up", "down", "select", "back"])
    p.add_argument("--delay", type=int, default=200, help="ms between presses")
    p.add_argument("--hold", type=int, default=100, help="ms to hold each click")
    p.set_defaults(func=cmd_press)

    p = sub.add_parser("screenshot")
    p.add_argument("out", type=str)
    p.set_defaults(func=cmd_screenshot)

    p = sub.add_parser("logs")
    p.add_argument("--duration", type=int, default=15)
    p.add_argument("--pbw", type=str, help="Install this pbw first (captures startup logs)")
    p.add_argument("--out", type=str)
    p.set_defaults(func=cmd_logs)

    p = sub.add_parser("kill")
    p.set_defaults(func=cmd_kill)

    p = sub.add_parser("test")
    p.add_argument("--duration", type=int, default=15)
    p.set_defaults(func=cmd_test)

    p = sub.add_parser("flow")
    p.add_argument("steps_json", type=str)
    p.set_defaults(func=cmd_flow)

    p = sub.add_parser("trace")
    p.add_argument("--duration", type=int, default=30)
    p.add_argument("--press", action="append", help="Schedule a press: sec:button (e.g. 10:select)")
    p.set_defaults(func=cmd_trace)

    p = sub.add_parser("docshots")
    p.set_defaults(func=cmd_docshots)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
