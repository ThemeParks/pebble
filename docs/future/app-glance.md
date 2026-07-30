# AppGlance — Implemented

> Shipped on branch `feat/app-glance`. The original design notes are kept
> below; see "What changed" at the end for the implementation summary.

## Follow-ups (not yet implemented)

- **Template countdowns**: render the hours rung with an AppGlance
  subtitle template predicate (`time_until(<close_epoch>)` → "closes in
  45 min", "opens in 2h" pre-open) instead of static "9 AM - 10 PM" —
  the launcher evaluates the template at render time, so the text stays
  continuously accurate with zero background execution.
- **Multi-day slice queue**: every published slice expires (waits after
  30 min, hours at that day's close), so after the last expiry the
  launcher shows only the app name — a user who doesn't open the app for
  weeks gets nothing rather than something stale ("absence over
  staleness"). AppGlance accepts up to 8 slices per reload, advancing as
  each expires, and the schedule endpoint already returns multi-week
  data (PKJS currently filters to today). Queueing ~a week of per-day
  hours slices (today's countdown, then tomorrow's hours expiring at
  tomorrow's close, ...) keeps the glance accurate for about a week
  untouched. Needs the wire payload to carry future days' hours. Beyond
  that horizon, only server-pushed timeline topics stay live
  (docs/future/timeline-pins.md).

## Concept

Use the Pebble AppGlance API to publish a glance slice when the app exits, so the launcher entry shows live-ish park info instead of a static app name. The launcher then re-markets the app every time the user scrolls past it — very few Pebble apps do this, and it costs almost nothing.

## What the slice should say

Priority order (first that applies):

1. **Park hours for the last-viewed park** — `"MK · 9 AM - 10 PM"`. We already cache hours in persist (`storage_get_park_hours`), so this works even offline. Format watch-side per `clock_is_24h_style()` via `format_clock_time` (format.c).
2. **Headline wait** — `"Space Mtn 45m · MK"` (shortest wait among favourites, else the park's shortest overall). Only fresh enough data (< 30 min) should be used; stale waits on the launcher would be worse than hours.
3. **Fallback** — no slice (default app name), e.g. first launch.

## API sketch

```c
// In deinit(), before app_event_loop returns:
static void prime_glance(AppGlanceReloadSession *session,
                         size_t limit, void *context) {
    if (limit < 1) return;
    const AppGlanceSlice slice = {
        .layout = {
            .icon = APP_GLANCE_SLICE_DEFAULT_ICON,
            .subtitle_template_string = s_glance_text,
        },
        // Expire when the hours stop being valid (park close), so a
        // stale glance never outlives its truth.
        .expiration_time = s_park_close_epoch,
    };
    app_glance_add_slice(session, slice);
}
...
app_glance_reload(prime_glance, NULL);
```

Notes:
- `subtitle_template_string` supports time-format templates — could render "opens in 2h" style countdowns without waking the app.
- Slice text budget is ~150 bytes; one slice is plenty.
- `expiration_time`: use the park's close time (parse from the cached "HH:MM" + today's date); after close, fall back to the plain app name rather than showing yesterday's hours.

## Watch-side changes

- `main.c` deinit: build the glance text from `g_park` / storage cache and call `app_glance_reload`. No PKJS involvement — everything needed is already on the watch.
- `format.c`: reuse `format_clock_time`; add a small helper to compose the glance line (host-testable, unit-test the truncation and the priority ladder).
- Per-destination builds: prefix with the park's short name; uber build: prefix with destination short name if it fits.

## Scope

Small. One new function in main.c, one pure helper in format.c + unit tests, no protocol changes. Test in the emulator by exiting the app and screenshotting the launcher.

## What changed

- **`src/c/format.{c,h}`**: pure, host-testable glance composer.
  - `format_glance_line(const glance_input_t *, use_24h, out, outsz)` walks
    the priority ladder and returns which rung it used
    (`GLANCE_RUNG_HOURS` → `"<Park> · <open> - <close>"`,
    `GLANCE_RUNG_WAIT` → `"<Ride> <N>m · <Park>"`, `GLANCE_RUNG_NONE` →
    empty string, publish nothing). Empty park name drops the
    prefix/suffix instead of rendering a dangling `" · "`.
  - `format_hours_current(now_min, open_min, close_min)` — the pure
    "are these hours still worth showing" decision on minutes-since-
    midnight, including the past-midnight-close wraparound. Pre-open
    counts as current (hours are informative before the gates open).
  - `GLANCE_WAIT_MAX_AGE_SECS` (30 min) — freshness cutoff for the wait
    rung, enforced both by the caller and inside the composer.
- **`src/c/main.c`**: `publish_glance()` runs in `deinit()` after
  `app_event_loop()` returns. It resolves the last-viewed park
  (`s_current_park`, falling back to the persisted last park in the
  per-destination builds; the uber build publishes nothing when exiting
  from a picker), pulls hours from `g_park` or the persist cache, does
  the HH:MM→minutes and close→epoch math (`mktime`, +1 day for
  past-midnight closes), picks the headline wait (shortest operating
  favourite, else shortest operating overall), and calls
  `app_glance_reload`. The slice expires at park close (hours rung) or
  now+30 min (wait rung); a session that adds no slice clears any
  previous glance. Logs `glance: <text>` / `glance: none` on every exit.
- **`tests/unit/test_format.c`**: covers both rungs (12h/24h), ladder
  precedence, the none case, empty park name, truncation safety, and
  `format_hours_current` (daytime, pre-open, after close, past-midnight
  close at 23:00/00:30/02:00).
- No PKJS or protocol changes, as designed.
