# Ride Alerts — Design Notes

## Concept

User-selected rides get a wrist vibration when their status changes. Not favourites (those pin to the top of the list) — a separate "alert" toggle for rides you're actively planning to go on.

## Key constraints (from Jamie)

**Geo-locked to the park.** Alerts only fire when the user is physically at the destination. Nobody wants their watch vibrating about Space Mountain while they're at home. The phone's location determines this — PKJS checks geofence before sending alert-worthy status changes to the watch.

**Not every status change matters.** The user is on Haunted Mansion having a great time — they don't need to know Tiana's went down. Alerts should be:
- **Opt-in per ride** via a separate "alert" toggle (not tied to favourites)
- **Only for rides the user has explicitly flagged** as "I want to ride this today"
- **Probably only Down→Operating** (ride came back up — actionable information: "go now before the line builds"). Down notifications are noise — you can't do anything about a ride being down except wait.

**Session-based, not persistent.** Alert selections could auto-clear at park close or after 24 hours. You're not always interested in the same rides — today's alert list is different from next week's.

## Architecture sketch

### Watch side
- New alert state in `storage.c`: per-park alert list (similar structure to favourites, separate key space)
- Settings view: new "ALERT RIDES" sub-menu alongside existing FAVE RIDES / FAVE SHOWS
- `park_view.c` or new `alerts.c`: after each data refresh, diff previous vs current status for alerted rides. If a ride transitions to Operating, fire `vibes_short_pulse()` and briefly show the ride name.
- Visual indicator: small bell icon or different highlight colour on alerted rides in the list

### PKJS side
- Geofence check: on each poll, compare phone location against destination coordinates (available from API). Only include alert metadata in the payload when inside the geofence (~1km radius of the destination).
- Could push the diff logic to PKJS instead of the watch — PKJS already has previous + current state from the cache. Send an extra field in the pipe payload: `ALERT|rideid|newstatus` lines after the normal data.

### API side
- No changes needed. Status data already includes Operating/Down/Closed. Destination coordinates available from `/v1/entity/{id}`.

## Open questions

- Should alerts also cover shows (show time approaching)?
- How to handle rapid flapping (ride goes down/up/down in 10 minutes)?
- Does the Pebble phone app expose location to PKJS, or do we need the Pebble location API?
- Should the alert list persist across app restarts within the same day, or truly be ephemeral?

## Scope

Medium project. New storage keys, new settings sub-menu, diff logic, vibe integration, PKJS geofence. The geofencing is the most uncertain part — depends on what location APIs PKJS has access to. Should be its own brainstorming cycle.
