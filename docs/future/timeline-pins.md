# Timeline Pins — Design Notes

## Concept

Push Pebble timeline pins so park info lives in the watch's past/future
timeline, extending the app's presence beyond explicit launches:

- **Park hours pin** for today at the last-used park: pin at opening time,
  "Magic Kingdom · 9 AM - 10 PM", with an "Open app" action.
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
