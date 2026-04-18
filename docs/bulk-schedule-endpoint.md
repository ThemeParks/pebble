# Bulk Schedule Optimization — Implemented

## Problem

At startup, PKJS made **one HTTP request per park** to `/entity/{id}/schedule` to get opening hours for the park picker (4 requests for WDW). Each returned the full multi-week schedule when we only needed today's hours.

## Solution

The ThemeParks.wiki API already supports fetching all park schedules in one request via the **destination** schedule endpoint:

```
GET /v1/entity/{destinationId}/schedule
```

Response includes a `parks` array with each child park's full schedule. PKJS now makes this single request instead of N per-park requests, filters to today's `OPERATING` hours, and sends the same pipe-delimited payload to the watch.

## What changed

- **`src/pkjs/index.js`**: Added `DESTINATION_ID` constant (patched per-destination by `build_dest.py`). Rewrote `fetchAllHours()` from N parallel `/entity/{parkId}/schedule` calls to one `/entity/{destinationId}/schedule` call. Filters response `parks` array against `ALL_PARK_IDS` to skip water parks etc.
- **`scripts/build_dest.py`**: `patched_pkjs()` now also injects `DESTINATION_ID` from `destinations/*.json`.

## Impact

- **Cold-start latency**: 4 HTTP round-trips → 1. Saves 1-3 seconds.
- **API load**: N× fewer requests per app startup.
- **Watch-side**: no changes — pipe-delimited `ALL_HOURS_DATA` format unchanged.
