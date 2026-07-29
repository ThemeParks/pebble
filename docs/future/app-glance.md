# AppGlance — Design Notes

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
