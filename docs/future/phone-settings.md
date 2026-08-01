# Phone-Side Settings Page — v1 Implemented

## Concept

A Pebble config page (the standard phone-side settings webview) for
settings that don't belong on a watch button: the AppGlance toggle and,
in the uber app, a fixed-destination mode. **v1 shipped the config-page
mechanism, the AppGlance on/off toggle, and the uber build's
Destination mode with a fixed-destination dropdown.** The My Parks
multi-select shipped next — see "My Parks (implemented)" below.

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

### Fixed-mode navigation (implemented after device testing)

Device testing showed the original v1 plan (leave watch nav alone) was
wrong: BACK from a fixed single-park destination dumped the user into
the destination picker they had explicitly opted out of. Fixed mode now
behaves exactly like a per-destination build:

- Every fixed-mode DEST_PARKS payload leads with an **`M|fixed`
  marker line** (`buildParkLines` in lib.js; parsed watch-side by
  `dest_parks_fixed_marker` in dest_parse.c). An unmarked payload
  (watch-side pick, or "choose on watch" restored) clears the mode.
- In fixed mode the destination picker is never shown: single-park BACK
  **exits the app** (publishing the glance on the way out); multi-park
  destinations use the park picker as the root window, so BACK from it
  exits too.
- PKJS answers **every** dest-list request AND overrides DEST_SELECT
  (including the watch's startup resume of its persisted last
  destination) with the fixed destination while the setting is active —
  launching always lands in the fixed destination. The old
  once-per-session serve guard is gone; the request loop it prevented
  cannot occur now that fixed mode never re-requests the list.

### v1 honest limitations

- A fixed `destId` that disappears from the API no longer self-heals
  within a session (every request is answered with the fixed serve,
  which sends nothing for an unknown id) — changing the setting on the
  phone recovers. Acceptable: destination ids are stable.
- Switching fixed → "choose on watch" while the watch app is running
  takes effect on the next destination pick or relaunch (no unmarked
  payload is pushed proactively).
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

## My Parks (multi-select, up to 4) — implemented

The parks whose live info the user wants surfaced OUTSIDE the app: a
packed multi-park wait line in the launcher glance. Selectable across
destinations in the uber build (a Florida local wants MK, EPCOT and
Islands of Adventure regardless of what they browse on-watch);
per-destination builds restrict the choice to their own parks.
Independent of Destination mode.

### Settings schema

`myParks: ["<parkUuid>", ...]` joined the settings JSON (still `v: 1` —
the field is additive and optional). `parseSettings` tolerates absence
(→ `[]`), drops junk entries (non-strings, empty strings, a non-array
wholesale), dedupes, and truncates past the cap (`MYPARKS_MAX = 4`).

The config page grew a "My Parks" checkbox section: the uber build
lists every destination's parks grouped by destination name
(alphabetical, from the cached `fetchDestList()`); per-destination
builds pass their own parks into `buildConfigPage(settings, dests,
isUber, parks)` — resolved from the dest list entry matching the
build's `DESTINATION_ID`, filtered to `ALL_PARK_IDS` (the API
destination may carry water parks the watch app doesn't ship). Page JS
disables the unchecked boxes once 4 are checked and the save handler
re-clamps. When the list can't be fetched the section degrades to a
hint, like the destination dropdown.

### Park codes (`lib.parkCode`)

The packed line needs 2–3 char park codes:

- Words starting with an uppercase letter or digit contribute their
  first character; lowercase-led connectives are skipped:
  "Magic Kingdom" → "MK", "Islands of Adventure" → "IA",
  "Epic Universe" → "EU", "Disney's Hollywood Studios" → "DHS".
- Codes cap at 3 chars ("Disney's Animal Kingdom Theme Park" → "DAK").
- Fewer than 2 qualifying initials (single-word names, lowercase-only
  names): first 3 alphanumeric characters uppercased — "EPCOT" → "EPC",
  "Efteling" → "EFT".
- Charset is [A-Z0-9] by construction — wire-safe (below).
- Honest limitation: codes aren't guaranteed unique — "Tokyo
  Disneyland" and "Tokyo DisneySea" both code to "TD", so that pairing
  reads as "TD 5m · TD 30m". Order still matches the selection order,
  and a smarter disambiguator wouldn't fit 3 chars anyway.

### Wire format

`SETTINGS_DATA` gained a `line` segment:
`glance=1;line=MK 45m · EP 30m|<expiryEpochSecs>`. The value splits on
its LAST `|`; the expiry must be 1–10 digits fitting uint32, else the
whole field parses as absent. The text is raw UTF-8 — no escaping
needed because `lib.buildGlanceLine` guarantees it never contains `;`,
`=` or `|` (park codes are [A-Z0-9], waits render as `<N>m`, the
separator is the UTF-8 middle dot, and a final strip enforces it).
Empty text (`line=|0`) is the explicit clear, pushed whenever
`myParks` is empty. Older watch binaries ignore the unknown segment.

The watch persists text + expiry (storage keys 4/5, RAM-cached,
empty-set clears) and `publish_glance()` makes an unexpired line the
FIRST slice — replacing the locally-composed single-park wait rung;
countdown and future-day slices still follow. It sits outside the
current-park guard, so uber picker exits (which publish no wait rung)
still show it. Absent/expired → exactly the old behaviour.

### Staleness model (the glance must never lie)

PKJS only runs while the watch app talks to it, so the packed line is a
snapshot of last app use, bounded three ways:

- PKJS keeps a per-park `{name, wait, ts}` snapshot (localStorage
  `tpw_waits`, `wait` = shortest operating wait, the same headline
  shape the watch composes), refreshed on every park-data cache save.
- Before each push it re-fetches any selected park whose cache is
  stale (max 4 requests through the normal `fetchAndFilter`/cache
  path), then `buildGlanceLine` drops entries older than 30 min.
- The pushed expiry is now + 30 min, and the watch re-checks it at
  publish time; the launcher's slice expiration retires it after that.

The line is (re)pushed after every `sendParkData` and on every settings
push (save + the ready handshake). Truly live background waits still
need server-pushed timeline topic pins (see timeline-pins.md) — out of
scope here.

### My Parks × timeline (still future)

The set should also drive timeline pins (per-park hours pins,
favourited showtimes — see docs/future/timeline-pins.md; pins remain
future work). Live WAIT times do not belong in pins.

## Future work

### Other deferred items

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
