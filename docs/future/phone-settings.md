# Phone-Side Settings Page — Design Notes

## Concept

A Pebble config page (the standard phone-side settings webview) with
three settings:

- **Destination mode** (uber app): "Choose on watch" (default — today's
  destination picker flow) vs "Fixed destination" + a dropdown of all
  destinations grouped by region. Fixed mode skips the watch's
  destination picker entirely: the park picker becomes the root window
  and BACK behaves like the per-destination builds.
- **My Parks** (multi-select, up to ~4): the parks whose live info the
  user wants surfaced OUTSIDE the app — in the launcher glance and on
  the timeline. Selectable across destinations (a Florida local wants
  MK, EPCOT and Islands of Adventure regardless of which destination
  they browse on-watch). Independent of Destination mode.
- **AppGlance toggle**: on/off for the launcher glance slice.

## Why (beyond convenience)

A fixed destination makes the uber app behave like a per-destination
build, which is the missing prerequisite for several features:

- **AppGlance**: the wait-headline rung needs a stable "current park";
  today the uber app publishes nothing when exiting via the pickers
  (`s_current_park == -1` and g_parks may not contain the persisted park
  after a destination switch). Fixed mode makes the persisted-last-park
  fallback safe in the uber build.
- **Timeline pins / ride alerts** (docs/future/): both need a stable
  "home destination" to pin against or geofence.
- It also sidesteps the slot-keyed favourites collision
  (docs/known-issues.md) for users who fix their destination.

## Mechanism

- package.json: `"capabilities": ["configurable"]` → gear icon in the
  phone's Pebble app. (build_dest.py's gen_package_json copies
  package.json wholesale, so all builds inherit it.)
- PKJS `showConfiguration` event → `Pebble.openURL(dataUri)`.
- **No hosting**: generate the page locally and open it as a `data:` URI
  (the Clay-library approach, hand-rolled to stay dependency-free). PKJS
  injects the cached destination list (fetchDestList) into the generated
  HTML as a region-grouped `<select>` — offline-capable and always
  current.
- On save the page navigates to `pebblejs://close#<encoded JSON>` →
  PKJS `webviewclosed` event → parse, persist to localStorage
  (`tpw_settings`), apply.

## Applying the settings

- **Fixed destination**: when the watch requests the destination list,
  PKJS answers with the DEST_PARKS_DATA flow for the fixed destination
  plus a "fixed" flag (new field or message key) so the watch skips
  stacking the destination picker. If the setting changes while the
  watch app is running, PKJS pushes the new destination immediately
  (same path as a watch-side DEST_SELECT). Watch-side: uber main.c gets
  a "fixed mode" branch — park picker as root, BACK-from-park matches
  the per-destination builds.
- **Glance toggle**: the glance publishes watch-side in deinit, so the
  watch must know the setting. PKJS pushes a small settings payload
  (e.g. `SETTINGS_DATA: "glance=0|1"`) after the ready handshake and on
  webviewclosed; the watch persists it (new storage key) and
  publish_glance() checks it — off publishes an empty session, which
  clears any existing slice.
- **My Parks glance line**: AppGlance shows ONE subtitle at a time
  (slices are an expiration-ordered queue, not a carousel), so
  multi-park means packing one line: "MK 45m · EP 30m · IoA 60m". The
  watch only ever holds one park's live data, so PKJS precomputes the
  packed line (headline wait per selected park from its cache, fetching
  the others' live endpoints on its normal serve path) and pushes it
  with the settings payload (e.g. `glanceline=<text>|<expiry>`). The
  watch persists it and publish_glance() prefers it over the
  locally-composed single-park line. Side effect: the glance then works
  even when the user exits from a picker (today's wait rung requires
  live in-app data). Budget: ~150 bytes subtitle; 4 parks × "XX 45m · "
  fits with short park codes — PKJS derives 2-3 letter codes from park
  names, or PARK_STYLE grows a `code` field.
- Per-destination builds show the glance toggle and a My Parks
  multi-select restricted to that destination's parks (PKJS knows the
  build flavour from `DESTINATION_ID != null`).

## Testing

- lib.js gets the pure parts: settings serialise/parse, config-page HTML
  generation (given a destination list + current settings), and the
  webviewclosed payload application — all covered by the node unit
  tests.
- Watch-side settings persistence → tests/unit/test_storage.c.
- Integration: `pebble emu-app-config` opens the config page against the
  emulator and can return a payload, so the webviewclosed → push →
  watch-persist chain is testable end to end.

## My Parks × timeline (see docs/future/timeline-pins.md)

The My Parks set is also the driver for timeline pins: per-park hours
pins for every selected park (not just the last-viewed one), plus
favourited showtimes. Live WAIT times do not belong in pins — pins are
time-anchored and can't update, so a wait number would be a fossil
within minutes; waits stay in the glance line, which refreshes every
time PKJS serves data. A possible middle ground is a morning "Today at
your parks" pin (hours for all My Parks in one body), refreshed daily.

## Open questions

- Should "Fixed destination" also pre-select a *park* (single-park
  destinations make this moot; multi-park could remember last park —
  which the watch already does)? Leaning no: fixed destination + watch
  park picker is the right split.
- My Parks glance-line freshness: PKJS only runs while the watch app
  talks to it, so the packed WAIT line is a snapshot of the last app use
  — stale within minutes, by platform design. The glance must never lie:
  waits get a hard expiry (~30 min) so the slice disappears rather than
  fossilise, and the durable rungs carry the everyday value — hours, and
  firmware-rendered countdowns via AppGlance subtitle template
  predicates (`time_until(<close_epoch>)` → "Closes in 45 min", rendered
  live by the launcher with zero background execution). Truly live
  background waits are only possible via server-pushed timeline topic
  pins (see timeline-pins.md) — a themeparks.wiki backend feature, out
  of scope here.
- Settings versioning: include a `v` field in the JSON from day one so
  later additions (units override, refresh cadence?) migrate cleanly.

## Scope

Medium. New PKJS module (page generation + settings), one new message
key, uber main.c fixed-mode branch, watch settings persist + glance
check, unit + integration tests. No API-side changes.
