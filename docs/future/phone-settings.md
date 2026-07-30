# Phone-Side Settings Page — v1 Implemented

## Concept

A Pebble config page (the standard phone-side settings webview) for
settings that don't belong on a watch button: the AppGlance toggle and,
in the uber app, a fixed-destination mode. **v1 (this iteration)
shipped the config-page mechanism, the AppGlance on/off toggle, and the
uber build's Destination mode with a fixed-destination dropdown.** The
My Parks multi-select remains future work (see below).

## What shipped (v1)

### Mechanism

- `package.json`: `"capabilities": ["configurable"]` → gear icon in the
  phone's Pebble app. (`build_dest.py`'s `gen_package_json` copies
  `package.json` wholesale, so all builds inherit it — verified in the
  generated `dist/*/package.json`.)
- **No hosting**: PKJS `showConfiguration` → `lib.buildConfigPage()`
  generates the page locally (hand-rolled, inline CSS/JS, ES5, no
  external resources — works offline) and opens it via
  `Pebble.openURL("data:text/html;charset=utf-8," + encodeURIComponent(html))`.
- Uber build: the page's destination dropdown is populated from the
  cached `fetchDestList()` (24h TTL), grouped into `<optgroup>`s by
  `lib.classifyRegion` (location-less destinations land in an "Other"
  group rather than being dropped), groups and names alphabetical. If
  the list can't be fetched the page still opens, with the
  fixed-destination option disabled and a hint.
- Save navigates to `pebblejs://close#<encodeURIComponent(JSON)>` →
  PKJS `webviewclosed` parses (`lib.parseSettings`, garbage-tolerant),
  persists to localStorage `tpw_settings`, and applies. Cancel/back
  closes with no payload → no change.

### Settings JSON schema (localStorage `tpw_settings` + close payload)

```json
{ "v": 1, "glance": true, "destMode": "watch", "destId": null }
```

- `v` — schema version, present from day one so later additions
  (My Parks, units override, …) migrate cleanly. Unknown fields are
  dropped on parse; missing/mistyped fields get defaults.
- `glance` — AppGlance on/off (default on).
- `destMode` — `"watch"` (default; today's destination picker flow) or
  `"fixed"`. `"fixed"` without a `destId` degrades to `"watch"`.
- `destId` — destination UUID when fixed, else null. Inert in
  per-destination builds.

### Wire format (phone → watch, `SETTINGS_DATA` message key)

Semicolon-separated `key=value` segments; v1 sends only what the watch
needs: `"glance=1"` / `"glance=0"`. The watch parser
(`src/c/settings_parse.c`, pure/host-tested) ignores unknown segments
and malformed values, so future fields append as `;key=value` without
breaking older watch binaries. Pushed on `webviewclosed` (settings
changed) **and** after the ready handshake on every launch, so the
watch persist stays in sync even when settings changed while the watch
app was closed.

### Watch side

- `storage.{c,h}`: `storage_get/set_glance_enabled` — persist key 3,
  RAM-cached lazy read, defaults to enabled when never set (upgrading
  users keep the behaviour they had).
- `api.c`: `SETTINGS_DATA` added to the inbox data-key probe chain;
  `dispatch_payload` parses via `settings_parse()` and applies only the
  fields the payload carries.
- `main.c` `publish_glance()`: when the glance is disabled it still
  runs an **empty** `app_glance_reload` — adding no slices is what
  clears a previously published glance — and logs "glance: disabled".

### Fixed destination (uber build)

- When the watch requests the destination list and settings say
  fixed+destId, PKJS answers the **first request of the session** with
  the fixed destination's parks (`handleDestSelect`, the
  DEST_PARKS_DATA flow) instead of the list, logging
  `PKJS: fixed destination <id>`. The watch's dest-list watchdog is
  cancelled on DEST_PARKS_DATA arrival (api.c — parks arriving means
  the request chain concluded; without this the watch would re-request
  forever, since no DEST_LIST_DATA ever comes).
- Once-only per PKJS session (i.e. per watch app launch), because the
  watch re-requests the list whenever its destination picker lacks
  data: answering every request with parks would loop
  (request → parks → show_picker → request), and a single-park fixed
  destination would re-load the park on every BACK, trapping the user.
  Later requests get the normal list, which keeps the destination
  picker browsable.
- If the setting changes while the watch app is running, PKJS pushes
  the new destination immediately (same path as a watch-side
  DEST_SELECT) and that counts as the session's fixed serve.

### v1 honest limitations

- **Watch nav in fixed mode is unchanged**: the destination picker
  remains reachable beneath the park picker, and picking a different
  destination there works for the rest of the session — the fixed
  destination re-asserts itself on the next watch app launch. Making
  the park picker the root window in fixed mode (BACK exits, like a
  per-destination build) is deferred; nav-flow surgery wasn't worth the
  risk before emulator verification.
- A watch-side `last_destination` (persisted by an on-watch pick)
  resumes via DEST_SELECT, which the fixed mode deliberately does not
  override — such users get their picked destination on resume until
  they clear it by navigating; users who never picked on-watch always
  launch into the fixed destination.
- A fixed `destId` that disappears from the API self-heals: the fixed
  serve sends nothing, the watch's watchdog re-requests, and the
  second request returns the normal list.
- Saving from a degraded page (dest list unavailable) writes
  `destMode: "watch"` — a previously fixed destination is cleared
  rather than preserved.

### Tests

- `tests/js/test_lib.js`: `parseSettings` (defaults, garbage, partials,
  unknown-field tolerance, version passthrough), `encodeSettingsPayload`,
  `buildConfigPage` (uber/non-uber sections, optgroup grouping/sorting
  incl. location-less fixture, empty-dest hint, prefill markers, close
  URLs, escaping, self-containment).
- `tests/unit/test_settings_parse.c`: payload parser (both values,
  garbage, truncated-length bounds, unknown/malformed segments).
- `tests/unit/test_storage.c`: glance flag default / roundtrip /
  persistence / RAM-cache contract.
- Emulator integration (`pebble emu-app-config` → webviewclosed → push
  → watch persist chain) remains outstanding — the standard end-to-end
  check once network access is available.

## Future work

### My Parks (multi-select, up to ~4) — deferred

The parks whose live info the user wants surfaced OUTSIDE the app — in
the launcher glance and on the timeline. Selectable across destinations
(a Florida local wants MK, EPCOT and Islands of Adventure regardless of
which destination they browse on-watch). Independent of Destination
mode. The settings JSON's `v` field exists so this can be added as e.g.
`myParks: [id, ...]` with a clean migration.

- **My Parks glance line**: AppGlance shows ONE subtitle at a time
  (slices are an expiration-ordered queue, not a carousel), so
  multi-park means packing one line: "MK 45m · EP 30m · IoA 60m". The
  watch only ever holds one park's live data, so PKJS precomputes the
  packed line and pushes it with the settings payload (e.g.
  `glanceline=<text>|<expiry>` — the wire format's `;key=value`
  extension point). The watch persists it and publish_glance() prefers
  it over the locally-composed single-park line. Budget: ~150 bytes
  subtitle; 4 parks × "XX 45m · " fits with short park codes.
- Freshness: PKJS only runs while the watch app talks to it, so a
  packed WAIT line is a snapshot of last app use. The glance must never
  lie: waits get a hard expiry (~30 min); durable rungs carry hours and
  firmware-rendered countdowns. Truly live background waits need
  server-pushed timeline topic pins (see timeline-pins.md) — out of
  scope here.
- Per-destination builds would restrict the multi-select to that
  destination's parks (PKJS knows the flavour from
  `DESTINATION_ID != null`).
- My Parks × timeline: the set also drives timeline pins (per-park
  hours pins, favourited showtimes — see docs/future/timeline-pins.md).
  Live WAIT times do not belong in pins.

### Other deferred items

- **Fixed-mode watch nav**: park picker as root window, BACK matching
  the per-destination builds (see v1 limitations above).
- **Units (km/mi) override**, refresh cadence, and similar — versioned
  settings JSON makes these cheap to add.
- Should "Fixed destination" also pre-select a *park*? Leaning no:
  fixed destination + watch park picker is the right split (the watch
  already remembers the last park).

## Why fixed destination matters (beyond convenience)

A fixed destination makes the uber app behave like a per-destination
build, which is the missing prerequisite for several features:

- **AppGlance**: the wait-headline rung needs a stable "current park";
  the uber app publishes nothing when exiting via the pickers
  (`s_current_park == -1` and g_parks may not contain the persisted park
  after a destination switch). Fixed mode makes the persisted-last-park
  fallback safe in the uber build.
- **Timeline pins / ride alerts** (docs/future/): both need a stable
  "home destination" to pin against or geofence.
- It also sidesteps the slot-keyed favourites collision
  (docs/known-issues.md) for users who fix their destination.
