#!/usr/bin/env bash
# End-to-end check: image builds, ./pebble build works, .pbw is produced,
# integration tests pass. Intended for local sanity and CI.
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")/.."

echo "=== [1/5] building image ==="
docker compose build pebble-dev

echo "=== [2/5] starting service ==="
./pebble down >/dev/null 2>&1 || true
./pebble up

echo "=== [3/5] building wdw ==="
./pebble build wdw
test -s builds/wdw.pbw || { echo "FAIL: builds/wdw.pbw missing or empty"; exit 1; }

echo "=== [4/5] running integration tests ==="
./pebble test

echo "=== [5/5] teardown ==="
./pebble down

echo "=== SMOKE TEST OK ==="
