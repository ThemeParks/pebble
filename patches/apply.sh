#!/usr/bin/env bash
# Apply pebble-tool patches. Run after SDK install or pebble-tool upgrade.
set -euo pipefail

SITE_PACKAGES="$(python3 -c 'import pebble_tool; import os; print(os.path.dirname(os.path.dirname(pebble_tool.__file__)))' 2>/dev/null || echo "$HOME/.local/share/uv/tools/pebble-tool/lib/python3.12/site-packages")"
PATCHES_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "Applying patches to: $SITE_PACKAGES"

# fetch_watch_info timeout tolerance
TARGET="$SITE_PACKAGES/libpebble2/communication/__init__.py"
if grep -q "continuing without watch info" "$TARGET" 2>/dev/null; then
    echo "  fetch-watch-info-timeout: already applied"
else
    patch -p1 -d "$SITE_PACKAGES" < "$PATCHES_DIR/pebble-tool-fetch-watch-info-timeout.patch"
    echo "  fetch-watch-info-timeout: applied"
fi

echo "Done."
