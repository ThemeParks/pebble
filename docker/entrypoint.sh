#!/usr/bin/env bash
# pebble-dev container entrypoint.
# - Starts Xvfb on :99 so the emulator always has a display.
# - Re-applies patches iff /work/.docker-patches-enabled exists
#   (set by `./pebble sdk-apply-patches`).
# - Execs the command (or sleeps if none given, for `docker compose up` mode).
set -euo pipefail

# Start Xvfb if not already running.
if ! pgrep -f 'Xvfb :99' >/dev/null 2>&1; then
    Xvfb :99 -screen 0 1024x768x24 -nolisten tcp &
    # Give Xvfb a moment to come up.
    for _ in 1 2 3 4 5; do
        if xdpyinfo -display :99 >/dev/null 2>&1; then break; fi
        sleep 0.2
    done
fi

# Opt-in SDK patch re-application. Marker is created by ./pebble sdk-apply-patches.
if [ -f /work/.docker-patches-enabled ] && [ -x /work/patches/apply.sh ]; then
    echo "[entrypoint] re-applying SDK patches (marker found)"
    /work/patches/apply.sh || echo "[entrypoint] patch re-apply failed (non-fatal)"
fi

if [ "$#" -eq 0 ]; then
    exec sleep infinity
fi

exec "$@"
