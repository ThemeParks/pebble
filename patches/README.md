# Pebble SDK Patches

Patches to the pebble-tool that must be re-applied after `pebble sdk install`
or `pip install pebble-tool`.

## In the Docker pipeline

Patches are **not** applied at image build. If you hit `TimeoutError: fetch_watch_info` from `pebble install`, run:

```bash
./pebble sdk-apply-patches
```

That runs `apply.sh` inside the container and writes `/work/.docker-patches-enabled`, which the container entrypoint uses to re-apply patches automatically on every start (so SDK-volume rebuilds don't lose the patched state).

## Host-native apply

```bash
./patches/apply.sh
```

## Patches

### `pebble-tool-fetch-watch-info-timeout.patch`

**Problem:** `pebble install --logs` crashes with `TimeoutError` on freshly
wiped emulators because `fetch_watch_info()` can't get a version response
from the firmware during its first-boot sequence.

**Fix:** Catch the timeout and continue with `_watch_info = None`. The install
and log streaming still work — only `watch_info`, `firmware_version`, and
`watch_platform` properties return None instead of real values.

**Target file:**
```
~/.local/share/uv/tools/pebble-tool/lib/python3.12/site-packages/libpebble2/communication/__init__.py
```
