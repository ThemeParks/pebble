# Timeline Pins — Implemented (opt-in)

## Concept

Push Pebble timeline pins so park info lives in the watch's past/future
timeline, extending the app's presence beyond explicit launches:

- **Park hours pin** per day at each of the user's parks ("Magic Kingdom
  hours · Open 09:00 - 22:00"). "The user's parks" = the My Parks
  multi-select when set, falling back to the last-served park otherwise.
- **Favourited show pins**: one pin per upcoming showtime of a
  favourited show ("Fantasmic! · 9:30 PM"). The user already told us
  they care — showtimes are exactly the kind of fixed-time event the
  timeline is for.

**Pins are OPT-IN and DEFAULT OFF** (product-owner requirement: "it
could be a bit spammy"). A "Timeline" card on the phone config page
carries the toggle, unchecked by default; NOTHING is pushed to the
timeline unless it's on, and turning it off deletes previously pushed
pins (best effort). The gate is checked first thing in
`syncTimelinePins` — with the toggle off a sync is one settings read,
no token fetch, no network.

## What shipped

### Settings + config page

- Settings JSON gains `pins: false` (additive, `v` stays 1;
  `parseSettings` treats anything but an explicit boolean as `false`,
  so nobody is ever opted in by accident or by garbage).
- Config page (`lib.buildConfigPage`): "Timeline" card with a checkbox
  ("Show park hours and favourite showtimes on the timeline") plus a
  hint that it needs the Rebble timeline service and that turning it
  off removes the pins. Prefills from settings; the save payload
  includes `s.pins`.
- The flag is **phone-only**: `encodeSettingsPayload` deliberately does
  not encode it — pins are pushed by PKJS via the timeline web API, the
  watch never acts on the flag.

### REST + token (index.js)

- `PUT https://timeline-api.rebble.io/v1/user/pins/<id>` with headers
  `X-User-Token` + `Content-Type: application/json`, body = pin JSON;
  `DELETE` on the same URL removes a pin (404 counts as done). New
  `xhrJson(method, url, headers, body)` alongside the GET-only
  `xhrGet`, 10s timeout, resolves `{status, text}`.
- `Pebble.getTimelineToken` once per session, cached; failure logs
  `PKJS: timeline token unavailable` once and disables pushes for the
  session (silent degrade — an unreachable timeline service costs one
  log line, never a retry storm, never the serve path).
- Test hook mirroring `tpw_test_location`: localStorage
  `tpw_test_timeline_token` overrides the token so the emulator harness
  (whose sandbox-token endpoint needs a developer-portal login) can
  exercise the push path.

### Pin composition (lib.js — pure, unit-tested)

- `buildHoursPin(parkId, parkName, openIso, closeIso)` →
  `{ id, time, duration?, layout: { type: "genericPin",
  title: "<parkName> hours", body: "Open <HH:MM> - <HH:MM>",
  tinyIcon: "system://images/TIMELINE_CALENDAR" } }`. Duration is
  open→close in minutes, clamped to [0, 24h] (omitted when 0/unknown).
- `buildShowPin(showFullId, showName, parkName, startIso)` →
  `{ id, time, layout: { type: "genericPin", title: showName,
  body: parkName, tinyIcon: "system://images/SCHEDULED_EVENT" } }`.
- `selectShowPins(liveData, favShortIds, nowMs, max)` — next `max`
  (default 3) FUTURE showtimes across the favourited shows in the RAW
  API live data (full entity ids matched against the `tpw_favs` short
  ids via `shortId`; RAW ISO startTimes), sorted by start.
- `pinDiff(prevIds, nextIds)` → `{toDelete, toKeep}` set difference for
  cleanup bookkeeping.
- `getUpcomingScheduleIso(schedule, maxDays, nowMs)` — same day
  selection as the glance's `getUpcomingSchedule` but keeping the RAW
  ISO strings; shares one `upcomingOperatingEntries` core.

### Pin id scheme (stable/idempotent)

- Hours: `tpw-hours-<shortId(parkId)>-<YYYY-MM-DD>` — the date sliced
  from `openIso`, i.e. the PARK-local date, so the same session maps to
  the same id whatever the phone's zone.
- Shows: `tpw-show-<shortId(showFullId)>-<epochMins>` — epoch minutes
  of the start instant: same showtime re-pushed = same id (the API
  updates in place), different showtime = distinct pin.
- Both well under the API's 64-char id cap; re-PUTting an id updates
  rather than duplicates, which is the whole idempotence story.

### Time zones

Pin `time` is UTC ISO (`new Date(iso).toISOString()`, ms stripped —
`isoToUtcIso`). The park API's ISO strings carry the park's UTC offset;
the zone-less wire `"HH:MM"` is display text and is NEVER used for pin
times (unit tests pin both a +09:00 Tokyo and a -04:00 Orlando
conversion). The park-local wall clock does appear — as text — in the
hours pin body.

### Sync engine (index.js)

- Trigger points: after every `sendParkData` serve (fire-and-forget in
  the sent-ok callback, behind the glance-line push) and after a
  `webviewclosed` settings save with pins on.
- Raw pin material (`pinSource`, in-memory): `fetchAndFilter` stashes,
  per park, the next 2 operating days with RAW ISO open/close and the
  RAW SHOW entities (full ids + ISO showtimes) before filtering strips
  them. Parks not fetched this session are skipped by the sync and
  caught up on their next fetch.
- Rate caps: hours pins for today + tomorrow per in-scope park, the
  next 3 favourited showtimes of the served park, ≤10 pins per sync.
- Bookkeeping: localStorage `tpw_pins` = `{pinId: expiryMs}` (hours:
  close time; shows: start + 1h). Expired entries prune locally — a
  past pin needs no DELETE. Recording is optimistic at attempt time, so
  toggle-off cleanup can only over-delete (a 404, treated as done).
- Cleanup:
  - Toggle off (true→false on save): DELETE every live id, clear the
    store, log `PKJS: pins disabled`.
  - Unfavourite (`FAVS_SYNC` diff old vs new state): DELETE that show's
    pins.
  - Per sync: `pinDiff` over the ids the sync is authoritative for
    (hours pins of in-scope parks, show pins of the served park's
    shows) — vanished showtimes lose their pins; out-of-scope pins
    (e.g. after an uber destination switch) are left to expire on
    their own.
- Logs (greppable): `PKJS: pin push <id>`, `PKJS: pin push failed <id>
  <status>`, `PKJS: pin delete <id>`, `PKJS: pins disabled`,
  `PKJS: timeline token unavailable`. All failures log once and never
  throw or block the serve path.

## Server-pushed topic pins (the only live-background channel)

Timeline sync is the one channel that updates without the watch app
running: the phone syncs pins from the timeline web service on its own
schedule (~15-30 min). Pins can be pushed server-side to shared topics
(`Pebble.timelineSubscribe("waits-<parkId>")` per My Park), and
ThemeParks.wiki owns the backend with the data — an hourly "park pulse"
pin (waits digest) per park topic is feasible. This is the only way to
beat the "glance is a snapshot of last app use" constraint, at the cost
of real server infrastructure (per-topic push jobs, rate limiting; the
timeline is not for high-frequency updates — hourly is the honest
ceiling). Separate opt-in feature if ever built; everything else in
this doc is now implemented without it.

## Still future

- Server-pushed topic pins (above).
- Pin actions (`openWatchApp` with a launchCode routing straight to the
  pinned park) — the API supports it; needs watch-side launch-reason
  handling.
- Deleting pins on uber destination switch (currently they just expire;
  hours pins die within ~a day anyway).
