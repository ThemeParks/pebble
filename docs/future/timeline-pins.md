# Timeline Pins — Design Notes

## Concept

Push Pebble timeline pins so park info lives in the watch's past/future
timeline, extending the app's presence beyond explicit launches:

- **Park hours pin** for today at each of the user's parks: pin at
  opening time, "Magic Kingdom · 9 AM - 10 PM", with an "Open app"
  action. "The user's parks" = the My Parks multi-select from the
  phone-side settings page (docs/future/phone-settings.md) when set —
  locals want several specific parks, not just the last-viewed one —
  falling back to the last-used park otherwise.
- **Favourited show pins**: one pin per upcoming showtime of a favourited
  show ("Fantasmic! · 9:30 PM"). The user already told us they care —
  showtimes are exactly the kind of fixed-time event the timeline is for.

## Architecture

Timeline pins are pushed from PKJS via the timeline web API (or
`Pebble.timelineSubscribe` for topics — not applicable here; we need
per-user pins).

- PKJS already has hours + showtimes in hand after every park fetch.
- Pin ids must be stable/idempotent (e.g. `hours-<parkId>-<date>`,
  `show-<showId>-<startTime>`) so re-fetches update rather than duplicate.
- Rate limits: the public timeline API allows a small number of pins per
  app per user — cap at hours + the next N (≈3) favourited showtimes.
- Watch → phone: PKJS needs to know which shows are favourited. Today
  favourites live only in watch persist. Add a small message
  (`FAV_SHOWS_SYNC`: comma-joined short ids) sent whenever show favourites
  change; PKJS caches it in localStorage. This sync is also a prerequisite
  for the geofenced ride alerts design (docs/future/ride-alerts.md).

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
this doc works without it.

## Caveats

- Timeline requires the Rebble/Core timeline service to be reachable from
  the phone app; degrade silently when it isn't.
- Time zones: pin times must be the park's local wall-clock converted to
  UTC — the ISO strings from the API carry the offset, so convert in PKJS
  before pushing (do NOT reuse the wire "HH:MM", which is deliberately
  zone-less).
- Cleanup: delete stale pins when the user switches destination (uber) or
  unfavourites a show.

## Scope

Medium. New PKJS module (pin composition — pure, unit-testable; push —
I/O), one new message key for favourite sync, storage hook on toggle.
No C UI changes.
