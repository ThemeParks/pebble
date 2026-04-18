#!/usr/bin/env python3
"""Capture Pebble app logs directly from pypkjs WebSocket.

Replaces `pebble logs` for environments where fetch_watch_info hangs
(e.g. after an emulator wipe). Uses libpebble2 directly.

Usage: python3 scripts/emu_logs.py [platform]   (default: emery)
"""
import glob
import json
import os
import sys
import tempfile
import time
from datetime import datetime
from pathlib import Path

# Use the pebble-tool's Python to get libpebble2. uv installs tools under
# $XDG_DATA_HOME/uv/tools (or ~/.local/share/uv/tools) with a versioned
# Python subdirectory — we glob rather than hard-coding the version.
_uv_root = os.environ.get(
    "XDG_DATA_HOME",
    str(Path.home() / ".local" / "share"),
)
for _p in glob.glob(os.path.join(_uv_root, "uv", "tools", "pebble-tool",
                                 "lib", "python*", "site-packages")):
    sys.path.insert(0, _p)
    break

from libpebble2.communication import PebbleConnection
from libpebble2.communication.transports.websocket import WebsocketTransport, MessageTargetPhone
from libpebble2.communication.transports.websocket.protocol import WebSocketPhoneAppLog
from libpebble2.protocol.logs import AppLogMessage, AppLogShippingControl


def get_pypkjs_port(platform="emery"):
    info_path = Path(tempfile.gettempdir()) / "pb-emulator.json"
    info = json.loads(info_path.read_text())
    versions = info.get(platform, {})
    ver = list(versions.keys())[0] if versions else None
    return versions[ver]["pypkjs"]["port"] if ver else None


def main():
    platform = sys.argv[1] if len(sys.argv) > 1 else "emery"
    port = get_pypkjs_port(platform)
    if not port:
        print(f"No pypkjs for {platform}", file=sys.stderr)
        return 1

    transport = WebsocketTransport(f"ws://localhost:{port}/")
    transport.connect()
    pebble = PebbleConnection(transport)
    pebble.run_async()

    # Skip fetch_watch_info — just subscribe to logs
    pebble.send_packet(AppLogShippingControl(enable=True))

    def on_watch_log(packet):
        ts = datetime.now().strftime("%H:%M:%S")
        print(f"[{ts}] {packet.filename}:{packet.line_number}> {packet.message}", flush=True)

    def on_phone_log(packet):
        ts = datetime.now().strftime("%H:%M:%S")
        print(f"[{ts}] pkjs> {packet.payload}", flush=True)

    pebble.register_endpoint(AppLogMessage, on_watch_log)
    pebble.register_transport_endpoint(MessageTargetPhone, WebSocketPhoneAppLog, on_phone_log)

    print(f"Listening on ws://localhost:{port}/  (Ctrl+C to stop)", file=sys.stderr)
    try:
        while pebble.connected:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    pebble.send_packet(AppLogShippingControl(enable=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
