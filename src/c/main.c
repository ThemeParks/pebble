/*
 * main.c — orchestrates the app lifecycle.
 *
 * Flow:
 *   init -> if persisted last_park: load it directly, else show park picker
 *   user picks park -> loading_view -> api_fetch_park -> on data: park_view
 *   in park_view: SELECT cycles waits/shows/settings; BACK pops back
 *   auto-refresh timer fires every 5min, re-fetches current park
 *
 * TEST_MODE: when the macro is defined at compile time, on app_init we kick
 * off an auto-switch loop that cycles through all 4 parks every 2s. Used
 * for soak testing memory stability.
 */

/* Define TEST_MODE at build time (e.g. via CFLAGS=-DTEST_MODE) to enable
 * the auto-switching soak harness in main.c — cycles parks every 2s. */

#include "config.h"
#include "parkdata.h"
#include "storage.h"
#include "format.h"
#include "api.h"
#include "park_picker.h"
#include "park_view.h"
#include "settings_view.h"
#include "loading_view.h"
#ifdef MULTI_DESTINATION
#include "dest_picker.h"
#include "dest_parse.h"
#endif
#include <pebble.h>
#include <string.h>

static int s_current_park = -1;
#ifdef MULTI_DESTINATION
/* True while the parks in g_parks came from the phone-side fixed
 * destination setting (M|fixed payload marker) — navigation then
 * matches a per-destination build: no destination picker, BACK exits. */
static bool s_fixed_mode = false;
#endif
static AppTimer *s_refresh_timer = NULL;
static AppTimer *s_fetch_watchdog = NULL;
/* True while an auto-refresh fetch is in flight. A silent refresh must be
 * invisible: on success the park view updates in place (same page, same
 * cursor row); on failure the user keeps the stale-but-usable data — the
 * header's age suffix tells them how old it is. Only user-initiated loads
 * get the loading screen / error screen treatment. */
static bool s_silent_refresh = false;
#define FETCH_WATCHDOG_MS 20000   /* 20s — beyond the api.c retry window */

#ifdef TEST_MODE
static AppTimer *s_test_timer = NULL;
static int s_test_switch_count = 0;
#define TEST_SWITCH_INTERVAL_MS 2000
#endif

static void on_park_data(bool ok);
static void load_park(int park_idx);
static void load_park_force(int park_idx);
static void schedule_refresh(void);
static void cancel_fetch_watchdog(void);
static void show_picker(void);
static void show_park(void);
static void show_settings(void);
static void show_error_for_current_park(void);
#ifdef MULTI_DESTINATION
static void on_dest_select(const char *dest_id, int park_count);
#endif

static void on_error_retry(void)
{
	if (s_current_park >= 0) load_park(s_current_park);
}

static void on_park_back(void);

static void on_error_back(void)
{
	/* Same exit as BACK from the park view — in the uber app a
	 * single-park destination must jump to the destination picker, not
	 * the one-item park picker. */
	on_park_back();
}

/* BACK on the plain loading screen: abort the in-flight fetch and fall
 * back to whichever picker is beneath. Without this the user is stuck
 * if a request dies silently (e.g. BT dropped mid-send). */
static void on_loading_abort(void)
{
	if (s_refresh_timer) { app_timer_cancel(s_refresh_timer); s_refresh_timer = NULL; }
	cancel_fetch_watchdog();
	s_current_park = -1;
	loading_view_hide();
}

static void watchdog_tick(void *ctx)
{
	s_fetch_watchdog = NULL;
	/* No response from PKJS within the watchdog window — show error. */
	show_error_for_current_park();
}

static void schedule_fetch_watchdog(void)
{
	if (s_fetch_watchdog) app_timer_cancel(s_fetch_watchdog);
	s_fetch_watchdog = app_timer_register(FETCH_WATCHDOG_MS, watchdog_tick, NULL);
}

static void cancel_fetch_watchdog(void)
{
	if (s_fetch_watchdog) {
		app_timer_cancel(s_fetch_watchdog);
		s_fetch_watchdog = NULL;
	}
}

/* --- timers ---------------------------------------------------------- */

static void refresh_tick(void *ctx)
{
	s_refresh_timer = NULL;
	if (s_current_park >= 0) {
		s_silent_refresh = true;
		/* Re-arm NOW rather than when the response lands — a lost reply
		 * (BT drop mid-transfer) must not silently kill every future
		 * auto-refresh. on_park_data() re-arms again on success, which
		 * just pushes the next tick out; that's fine. */
		schedule_refresh();
		api_fetch_park(g_parks[s_current_park].id);
	}
}

static void schedule_refresh(void)
{
	if (s_refresh_timer) app_timer_cancel(s_refresh_timer);
	s_refresh_timer = app_timer_register(REFRESH_INTERVAL_MS, refresh_tick, NULL);
}

#ifdef TEST_MODE
static void test_tick(void *ctx)
{
	s_test_timer = NULL;
	s_test_switch_count++;
	int next = s_test_switch_count % NUM_PARKS;
	APP_LOG(APP_LOG_LEVEL_INFO, "TEST: switch #%d -> %s",
	        s_test_switch_count, g_parks[next].name);
	load_park(next);
	s_test_timer = app_timer_register(TEST_SWITCH_INTERVAL_MS, test_tick, NULL);
}
#endif

/* --- screen transitions --------------------------------------------- */

static void on_picker_select(int park_idx)
{
	load_park(park_idx);
}

static void show_picker(void)
{
	if (s_refresh_timer) { app_timer_cancel(s_refresh_timer); s_refresh_timer = NULL; }
	cancel_fetch_watchdog();
	/* No park is current while browsing a picker — this also makes
	 * on_park_data() drop any response that was still in flight when the
	 * user backed out, instead of yanking the park view back on top. */
	s_current_park = -1;
	park_view_hide();
	settings_view_hide();
	loading_view_hide();
#ifdef MULTI_DESTINATION
	/* Keep the destination picker on the stack beneath the park picker
	 * so BACK from the park picker returns to the destination list
	 * instead of exiting the app. Without this, a user who resumed into
	 * a multi-park destination could never reach the destination list
	 * again (relaunch auto-resumes the same destination).
	 *
	 * Fixed-destination mode skips all of that: the user chose their
	 * destination on the phone, so the park picker is the root window
	 * and BACK from it exits like a per-destination build. */
	if (!s_fixed_mode) {
		dest_picker_show(on_dest_select);
		if (!dest_picker_has_data()) {
			api_request_dest_list();
		}
	}
#endif
	park_picker_show(on_picker_select);
}

static void show_error_for_current_park(void)
{
	const char *name = (s_current_park >= 0)
		? g_parks[s_current_park].name : NULL;
	loading_view_show_error(name, on_error_retry, on_error_back);
}

static void on_park_back(void)
{
#ifdef MULTI_DESTINATION
	/* Fixed destination + single park: there is nothing to go "back"
	 * to — no picker would offer a choice — so BACK exits the app,
	 * exactly like the per-destination builds (and deinit publishes
	 * the glance on the way out). */
	if (s_fixed_mode && num_parks == 1) {
		window_stack_pop_all(true);
		return;
	}
	/* Single-park destination: the park picker would only contain the
	 * one park we just came from, so jump straight to the destination
	 * picker instead. Multi-park destinations fall through to the
	 * normal park picker. */
	if (num_parks == 1) {
		/* Mirror show_picker(): kill the auto-refresh timer and fetch
		 * watchdog, and clear the current park so a late park-data
		 * response can't push the park view back over the picker. */
		if (s_refresh_timer) { app_timer_cancel(s_refresh_timer); s_refresh_timer = NULL; }
		cancel_fetch_watchdog();
		s_current_park = -1;
		park_view_hide();
		loading_view_hide();
		settings_view_hide();
		dest_picker_show(on_dest_select);
		api_request_dest_list();
		return;
	}
#endif
	show_picker();
}

static void on_settings_back(void)
{
	show_park();
}

static void on_park_settings(void)
{
	show_settings();
}

static void show_park(void)
{
	if (s_current_park < 0) { show_picker(); return; }
	loading_view_hide();
	settings_view_hide();
	park_view_show(g_parks[s_current_park].id, on_park_back, on_park_settings);
}

static void on_settings_refresh(void)
{
	/* Manual refresh from settings: close settings and reload the park
	 * with the phone-cache bypass — the user explicitly asked for
	 * now-data (e.g. just walked out of a queue). */
	if (s_current_park < 0) return;
	settings_view_hide();
	load_park_force(s_current_park);
}

static void show_settings(void)
{
	if (s_current_park < 0) return;
	settings_view_show(g_parks[s_current_park].id, on_settings_back,
	                   on_settings_refresh);
}

#ifdef MULTI_DESTINATION

static void on_dest_parks_received(const char *data, uint16_t len)
{
	/* An "M|fixed" first line means PKJS served these parks because of
	 * the phone-side fixed-destination setting — the watch then behaves
	 * like a per-destination build: no destination picker anywhere,
	 * BACK exits. A payload without the marker (watch-side pick, or
	 * settings back on "choose on watch") restores normal navigation. */
	uint16_t pos = 0;
	s_fixed_mode = dest_parks_fixed_marker(data, len, &pos);

	/* Parse "id|name|icon|color\n..." into g_parks[] */
	num_parks = 0;
	while (pos < len && num_parks < MAX_PARKS) {
		uint16_t nl = pos;
		while (nl < len && data[nl] != '\n') nl++;
		uint16_t line_len = nl - pos;
		if (line_len < 3) { pos = nl + 1; continue; }

		/* Parse 4 pipe-delimited fields: id|name|icon|color */
		const char *line = data + pos;
		const char *fields[4] = {NULL, NULL, NULL, NULL};
		uint16_t flens[4] = {0, 0, 0, 0};
		int fi = 0;
		const char *fs = line;
		for (uint16_t i = 0; i <= line_len && fi < 4; i++) {
			if (i == line_len || line[i] == '|') {
				fields[fi] = fs;
				flens[fi] = (uint16_t)(line + i - fs);
				fi++;
				fs = line + i + 1;
			}
		}
		if (fi >= 4) {
			park_config_t *p = &g_parks[num_parks];

			int id_len = flens[0] < sizeof(p->id_buf) - 1 ? flens[0] : (int)sizeof(p->id_buf) - 1;
			memcpy(p->id_buf, fields[0], id_len);
			p->id_buf[id_len] = '\0';
			p->id = p->id_buf;

			int nm_len = flens[1] < sizeof(p->name_buf) - 1 ? flens[1] : (int)sizeof(p->name_buf) - 1;
			memcpy(p->name_buf, fields[1], nm_len);
			p->name_buf[nm_len] = '\0';
			p->name = p->name_buf;

			int ic_len = flens[2] < sizeof(p->icon_buf) - 1 ? flens[2] : (int)sizeof(p->icon_buf) - 1;
			memcpy(p->icon_buf, fields[2], ic_len);
			p->icon_buf[ic_len] = '\0';
			p->icon = p->icon_buf;

			int cl_len = flens[3] < sizeof(p->color_buf) - 1 ? flens[3] : (int)sizeof(p->color_buf) - 1;
			memcpy(p->color_buf, fields[3], cl_len);
			p->color_buf[cl_len] = '\0';
			p->color = p->color_buf;

			num_parks++;
		}
		pos = nl + 1;
	}
	APP_LOG(APP_LOG_LEVEL_INFO, "populated %d parks from destination", num_parks);

	/* g_parks slots now mean different parks — drop slot-keyed caches
	 * (and the persisted hours, which belonged to the old destination). */
	storage_on_parks_changed();

	/* PKJS pushes hours unprompted after DEST_PARKS_DATA (via its
	 * handleDestSelect .then(fetchAllHours) chain). We don't need to
	 * call api_fetch_all_hours here — and doing so races with the
	 * outbox when we immediately load_park below. */

	if (num_parks == 1) {
		load_park(0);
	} else if (num_parks > 0) {
		/* show_picker() keeps the destination picker stacked beneath
		 * the park picker, so don't hide it here — BACK from the park
		 * picker must land on the destination list. */
		loading_view_hide();
		show_picker();
	}
}

static void on_dest_list_received(const char *data, uint16_t len)
{
	dest_picker_set_data(data, len);
}

static void on_dest_select(const char *dest_id, int park_count)
{
	(void)park_count;  /* unused — we count as we parse on_dest_parks_received */
	storage_set_last_destination(dest_id);
	loading_view_show("Loading parks...", on_loading_abort);
	api_select_destination(dest_id);
}

/* Aborting the resume-last-destination load at startup: fall back to
 * the destination picker rather than exiting to the watchface (there is
 * no window beneath the loading screen yet). */
static void on_startup_loading_back(void)
{
	cancel_fetch_watchdog();
	s_current_park = -1;
	dest_picker_show(on_dest_select);
	api_request_dest_list();
	loading_view_hide();
}

#endif

/* --- park loading --------------------------------------------------- */

static void on_park_data(bool ok)
{
	cancel_fetch_watchdog();
	bool silent = s_silent_refresh;
	s_silent_refresh = false;
	/* Data (or an error) for a park we've already navigated away from —
	 * e.g. the user pressed BACK to a picker while the fetch was in
	 * flight. Don't let it steal the screen. */
	if (s_current_park < 0) return;
	if (!ok) {
		if (silent) {
			/* Background refresh failed — keep the stale-but-usable data
			 * on screen. The header age suffix shows its age, and the
			 * next auto-refresh is already scheduled. */
			APP_LOG(APP_LOG_LEVEL_WARNING, "silent refresh failed - keeping stale data");
			return;
		}
		if (!api_is_connected()) {
			APP_LOG(APP_LOG_LEVEL_WARNING, "PKJS not connected");
			if (s_current_park >= 0) {
				loading_view_show_disconnected(on_error_retry, on_error_back);
			}
		} else {
			APP_LOG(APP_LOG_LEVEL_WARNING, "Park fetch failed");
			show_error_for_current_park();
		}
		return;
	}
	if (s_current_park >= 0) {
		parkdata_set_park_name(g_parks[s_current_park].name);
		/* Cache the freshly-fetched hours so the park picker can show
		 * them under the name on subsequent visits. */
		if (g_park.has_schedule) {
			storage_set_park_hours(g_parks[s_current_park].id,
			                       g_park.schedule_open,
			                       g_park.schedule_close);
		}
	}
	if (silent) {
		/* In-place update: same page, cursor back on the same ride. Also
		 * never pops windows, so a user sitting in settings stays there. */
		park_view_refresh();
		APP_LOG(APP_LOG_LEVEL_INFO, "silent refresh applied");
	} else {
		show_park();
		park_view_data_changed();
	}
	schedule_refresh();
}

static void load_park_ex(int park_idx, bool force)
{
	if (park_idx < 0 || park_idx >= NUM_PARKS) return;
	/* A user-initiated load supersedes any in-flight auto-refresh — its
	 * response (if it ever lands) must get the foreground treatment. */
	s_silent_refresh = false;
	s_current_park = park_idx;
	storage_set_last_park(g_parks[park_idx].id);

	/* Clear stale data so the loading screen header is empty */
	parkdata_clear();
	parkdata_set_park_name(g_parks[park_idx].name);

	loading_view_show(g_parks[park_idx].name, on_loading_abort);
	schedule_fetch_watchdog();
	if (force) api_fetch_park_force(g_parks[park_idx].id);
	else api_fetch_park(g_parks[park_idx].id);
}

static void load_park(int park_idx)
{
	load_park_ex(park_idx, false);
}

static void load_park_force(int park_idx)
{
	load_park_ex(park_idx, true);
}

/* --- app glance ------------------------------------------------------ */

/* The slice queue lives in statics because subtitle_template_string is
 * only read when the reload session commits, after prime_glance returns
 * — stack buffers would be gone by then. 8 matches the AppGlance
 * per-app slice cap; 96 bytes covers the longest composed countdown
 * template ("Disney's Hollywood Studios · closes in
 * {time_until(4294967295)|format('%aR')}" is 77 chars) — a template
 * truncated mid-brace would render literally, so the slack matters. */
#define GLANCE_MAX_SLICES 8
#define GLANCE_SLICE_LEN 96
static char s_glance_text[GLANCE_MAX_SLICES][GLANCE_SLICE_LEN];
static time_t s_glance_expiry[GLANCE_MAX_SLICES];
static int s_glance_count;

/* Strict wire "HH:MM" → minutes since midnight, or -1. Mirrors the
 * parse in format_clock_time; a -1 here means a legacy cached string
 * ("9 AM") we can't do time math on. */
static int glance_hhmm_to_min(const char *hhmm)
{
	if (!hhmm
	    || hhmm[0] < '0' || hhmm[0] > '9'
	    || hhmm[1] < '0' || hhmm[1] > '9'
	    || hhmm[2] != ':'
	    || hhmm[3] < '0' || hhmm[3] > '9'
	    || hhmm[4] < '0' || hhmm[4] > '9'
	    || hhmm[5] != '\0') return -1;
	int hh = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
	int mm = (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
	if (hh > 23 || mm > 59) return -1;
	return hh * 60 + mm;
}

/* Minutes-since-midnight on today's date → epoch. */
static time_t glance_min_to_epoch(time_t now, int min)
{
	struct tm t = *localtime(&now);
	t.tm_hour = min / 60;
	t.tm_min = min % 60;
	t.tm_sec = 0;
	return mktime(&t);
}

/* Today's close as an epoch — the closing slice's expiration, so a
 * stale glance never outlives its truth. */
static time_t glance_close_epoch(time_t now, int now_min,
                                 int open_min, int close_min)
{
	struct tm t = *localtime(&now);
	t.tm_hour = close_min / 60;
	t.tm_min = close_min % 60;
	t.tm_sec = 0;
	/* Past-midnight close (close < open): unless we're already in the
	 * small hours before it (now < close), the close lands on tomorrow's
	 * date — bump the day and let mktime normalise any month rollover. */
	if (close_min < open_min && now_min >= close_min) t.tm_mday += 1;
	return mktime(&t);
}

/* Strict "YYYY-MM-DD" (the shape parkdata.c already enforced on F|
 * lines — re-checked here because persist could someday feed this too). */
static bool glance_parse_date(const char *s, int *y, int *mo, int *d)
{
	for (int i = 0; i < 10; i++) {
		if (i == 4 || i == 7) {
			if (s[i] != '-') return false;
		} else if (s[i] < '0' || s[i] > '9') {
			return false;
		}
	}
	if (s[10] != '\0') return false;
	*y  = (s[0] - '0') * 1000 + (s[1] - '0') * 100
	    + (s[2] - '0') * 10 + (s[3] - '0');
	*mo = (s[5] - '0') * 10 + (s[6] - '0');
	*d  = (s[8] - '0') * 10 + (s[9] - '0');
	return *mo >= 1 && *mo <= 12 && *d >= 1 && *d <= 31;
}

/* Minutes-since-midnight on an arbitrary date (+day_offset for closes
 * that roll past midnight) → epoch. Starting from localtime(now) keeps
 * the DST guess consistent with the rest of this file; mktime
 * normalises any month/year rollover from the offset. */
static time_t glance_date_epoch(time_t now, int y, int mo, int d,
                                int min, int day_offset)
{
	struct tm t = *localtime(&now);
	t.tm_year = y - 1900;
	t.tm_mon = mo - 1;
	t.tm_mday = d + day_offset;
	t.tm_hour = min / 60;
	t.tm_min = min % 60;
	t.tm_sec = 0;
	return mktime(&t);
}

/* Headline attraction: shortest wait among operating favourites; if the
 * user has no operating favourites, shortest operating overall. */
static const attraction_t *glance_headline(const char *park_id)
{
	const attraction_t *best_fav = NULL, *best_any = NULL;
	for (int i = 0; i < g_park.attraction_count; i++) {
		const attraction_t *a = &g_park.attractions[i];
		if (a->status != 'O' || a->wait < 0) continue;
		if (!best_any || a->wait < best_any->wait) best_any = a;
		if (storage_is_favourite(park_id, FAV_ATTRACTIONS, a->id)
		    && (!best_fav || a->wait < best_fav->wait)) best_fav = a;
	}
	return best_fav ? best_fav : best_any;
}

static void prime_glance(AppGlanceReloadSession *session,
                         size_t limit, void *context)
{
	(void)context;
	/* Slices go in queue order: the launcher shows the first live one
	 * and advances as each expires. A zero-slice session is deliberate —
	 * adding nothing is what clears any previous glance back to the
	 * plain app name. */
	size_t n = (size_t)s_glance_count;
	if (n > limit) n = limit;
	for (size_t i = 0; i < n; i++) {
		const AppGlanceSlice slice = {
			.layout = {
				.icon = APP_GLANCE_SLICE_DEFAULT_ICON,
				.subtitle_template_string = s_glance_text[i],
			},
			.expiration_time = s_glance_expiry[i],
		};
		AppGlanceResult res = app_glance_add_slice(session, slice);
		if (res != APP_GLANCE_RESULT_SUCCESS) {
			APP_LOG(APP_LOG_LEVEL_WARNING,
			        "glance: add_slice %d failed (%d)", (int)i, (int)res);
		}
	}
}

/* Build the glance slice queue for the last-viewed park and hand it to
 * the launcher. Called from deinit(), i.e. every app exit. Queue order
 * (earlier = shown first, launcher advances as each expires):
 *   1. the phone-composed My Parks line ("MK 45m · EP 30m", expires at
 *      its pushed expiry) — when present and unexpired it REPLACES the
 *      locally-composed single-park wait rung below
 *   2. headline wait            (expires now+30m; skipped when 1 ran)
 *   3. "opens in ..." countdown (pre-open only; expires at open)
 *   4. "closes in ..." countdown (expires at close)
 *   5+. upcoming days "Wed · 9 AM - 10 PM" (each expires at its close)
 */
static void publish_glance(void)
{
	s_glance_count = 0;

	/* Toggled off from the phone's config page: still run an (empty)
	 * reload — adding no slices is what CLEARS a previously published
	 * glance; returning without reloading would fossilise it. */
	if (!storage_get_glance_enabled()) {
		APP_LOG(APP_LOG_LEVEL_INFO, "glance: disabled");
		app_glance_reload(prime_glance, NULL);
		return;
	}

	time_t now = time(NULL);
	struct tm lt = *localtime(&now);
	int now_min = lt.tm_hour * 60 + lt.tm_min;
	/* Park dates on the wire are "YYYY-MM-DD" — same shape here so the
	 * today/future split is a plain strcmp. */
	char today[DATE_STR_LEN];
	strftime(today, sizeof(today), "%Y-%m-%d", &lt);

	/* My Parks line (pushed by PKJS, persisted via SETTINGS_DATA): while
	 * its expiry is still ahead it is the first slice, replacing the
	 * locally-composed single-park wait rung — it carries the same class
	 * of data (fresh waits) for the parks the user actually asked for.
	 * It deliberately sits OUTSIDE the idx guard: the uber build's
	 * picker exits (no current park) can still show it. Expired/absent
	 * lines leave everything exactly as before. */
	bool line_used = false;
	{
		char line[GLANCE_LINE_MAX];
		uint32_t line_exp = 0;
		if (storage_get_glance_line(line, &line_exp)
		    && (time_t)line_exp > now) {
			/* The line is plain text (no '{' template syntax) well under
			 * GLANCE_SLICE_LEN, so a straight copy is safe. */
			snprintf(s_glance_text[s_glance_count], GLANCE_SLICE_LEN,
			         "%s", line);
			s_glance_expiry[s_glance_count] = (time_t)line_exp;
			s_glance_count++;
			line_used = true;
		}
	}

	int idx = s_current_park;
#ifndef MULTI_DESTINATION
	/* Exited from a picker (s_current_park == -1): fall back to the
	 * persisted last park so the launcher can still show its cached
	 * hours. In the uber build we skip this — after a destination
	 * switch g_parks may not even contain the persisted park id. */
	if (idx < 0) {
		const char *last = storage_get_last_park();
		if (last) {
			for (int i = 0; i < NUM_PARKS; i++) {
				if (strcmp(g_parks[i].id, last) == 0) { idx = i; break; }
			}
		}
	}
#endif

	if (idx >= 0) {
		const char *park_id = g_parks[idx].id;
		const char *park_name = g_parks[idx].name;
		bool use_24h = clock_is_24h_style();

		/* Headline wait: only from live data for this park, only fresh
		 * enough that the launcher won't show a fossil. When it expires
		 * (30 min) the launcher advances to the countdowns. Skipped when
		 * the My Parks line took this rung's slot above. */
		if (!line_used && idx == s_current_park && g_park.last_updated_secs != 0
		    && now >= (time_t)g_park.last_updated_secs) {
			uint32_t age = (uint32_t)now - g_park.last_updated_secs;
			const attraction_t *a = (age < GLANCE_WAIT_MAX_AGE_SECS)
				? glance_headline(park_id) : NULL;
			if (a && s_glance_count < GLANCE_MAX_SLICES) {
				glance_input_t in = {
					.park_name = park_name,
					.ride_name = a->name,
					.wait_mins = a->wait,
					.wait_age_secs = age,
				};
				if (format_glance_line(&in, use_24h,
				                       s_glance_text[s_glance_count],
				                       GLANCE_SLICE_LEN)
				    == GLANCE_RUNG_WAIT) {
					s_glance_expiry[s_glance_count] =
						now + GLANCE_WAIT_MAX_AGE_SECS;
					s_glance_count++;
				}
			}
		}

		/* Today's countdown slice(s). Hours: prefer the live fetch
		 * (g_park only holds this park's data when it's still current),
		 * else the persisted cache — which works offline and when the
		 * fetch never completed. Any parse failure (legacy cached
		 * strings like "9 AM") → no time math possible, no countdown. */
		char open_buf[PARK_HOURS_STR_LEN], close_buf[PARK_HOURS_STR_LEN];
		const char *open_s = NULL, *close_s = NULL;
		if (idx == s_current_park && g_park.has_schedule) {
			open_s = g_park.schedule_open;
			close_s = g_park.schedule_close;
		} else if (storage_get_park_hours(park_id, open_buf, close_buf)) {
			open_s = open_buf;
			close_s = close_buf;
		}
		if (open_s && close_s) {
			int open_min = glance_hhmm_to_min(open_s);
			int close_min = glance_hhmm_to_min(close_s);
			if (open_min >= 0 && close_min >= 0
			    && format_hours_current(now_min, open_min, close_min)) {
				/* Pre-open slice — only on a same-day schedule: with a
				 * past-midnight close, "current && before open" means
				 * the small hours of the PREVIOUS operating day, where
				 * the park is open, not opening. */
				if (close_min > open_min && now_min < open_min
				    && s_glance_count < GLANCE_MAX_SLICES) {
					time_t open_epoch =
						glance_min_to_epoch(now, open_min);
					if (open_epoch > now) {
						format_glance_countdown(park_name, "opens",
							(uint32_t)open_epoch,
							s_glance_text[s_glance_count],
							GLANCE_SLICE_LEN);
						s_glance_expiry[s_glance_count] = open_epoch;
						s_glance_count++;
					}
				}
				time_t close_epoch = glance_close_epoch(now, now_min,
				                                        open_min,
				                                        close_min);
				if (close_epoch > now
				    && s_glance_count < GLANCE_MAX_SLICES) {
					format_glance_countdown(park_name, "closes",
						(uint32_t)close_epoch,
						s_glance_text[s_glance_count],
						GLANCE_SLICE_LEN);
					s_glance_expiry[s_glance_count] = close_epoch;
					s_glance_count++;
				}
			}
		}

		/* Upcoming days — only from the exited session's live data.
		 * s_current_park is -1 on every BACK-BACK exit (each hop to a
		 * picker clears it as a view-stealing guard), but g_park still
		 * holds the last-viewed park's F| entries — and in the builds
		 * that reach here with a fallback idx, that park IS the
		 * persisted last park (load_park set both). The park name (set
		 * alongside every load/parse, truncated to the store's slot) is
		 * the surviving identity check. A session that never fetched
		 * this park's data has future_day_count == 0 and publishes
		 * nothing beyond today — matching the previous behaviour for
		 * persisted-hours-only exits. */
		bool session_is_this_park = (idx == s_current_park)
			|| (g_park.future_day_count > 0
			    && strncmp(g_park.park_name, park_name,
			               sizeof(g_park.park_name) - 1) == 0);
		if (session_is_this_park) {
			for (int i = 0; i < g_park.future_day_count
			     && s_glance_count < GLANCE_MAX_SLICES; i++) {
				const future_day_t *fd = &g_park.future_days[i];
				/* Dedupe convention (see parkdata.c): PKJS includes
				 * today in F|; line 0 already covered it above, so
				 * only dates strictly after today contribute here. */
				if (strcmp(fd->date, today) <= 0) continue;
				int y, mo, d;
				if (!glance_parse_date(fd->date, &y, &mo, &d)) continue;
				int fo = glance_hhmm_to_min(fd->open);
				int fc = glance_hhmm_to_min(fd->close);
				if (fo < 0 || fc < 0) continue;

				/* Weekday label from the date itself. Noon dodges any
				 * DST-shift ambiguity; mktime fills tm_wday for %a. */
				struct tm dt = lt;
				dt.tm_year = y - 1900;
				dt.tm_mon = mo - 1;
				dt.tm_mday = d;
				dt.tm_hour = 12;
				dt.tm_min = 0;
				dt.tm_sec = 0;
				mktime(&dt);
				char wkday[8];
				strftime(wkday, sizeof(wkday), "%a", &dt);

				/* Reuse the hours-rung composer: park slot carries the
				 * weekday → "Wed · 9 AM - 10 PM". */
				glance_input_t din = {
					.park_name = wkday,
					.has_schedule = true,
					.open_hhmm = fd->open,
					.close_hhmm = fd->close,
					.hours_current = true,
					.wait_mins = -1,
				};
				if (format_glance_line(&din, use_24h,
				                       s_glance_text[s_glance_count],
				                       GLANCE_SLICE_LEN)
				    != GLANCE_RUNG_HOURS) continue;
				/* Expire at that day's close; past-midnight closes
				 * roll to the next calendar day like today's logic. */
				time_t exp = glance_date_epoch(now, y, mo, d, fc,
				                               (fc < fo) ? 1 : 0);
				if (exp <= now) continue;
				s_glance_expiry[s_glance_count] = exp;
				s_glance_count++;
			}
		}
	}

	if (s_glance_count > 0) {
		APP_LOG(APP_LOG_LEVEL_INFO, "glance: %d slices, first: %s",
		        s_glance_count, s_glance_text[0]);
	} else {
		APP_LOG(APP_LOG_LEVEL_INFO, "glance: none");
	}
	/* Always reload — a session that adds no slice clears any previous
	 * one, so an outdated glance can't survive an exit that produced
	 * nothing. */
	app_glance_reload(prime_glance, NULL);
}

/* --- app lifecycle -------------------------------------------------- */

static void init(void)
{
#ifdef MULTI_DESTINATION
	APP_LOG(APP_LOG_LEVEL_INFO, "uber starting");
#else
	APP_LOG(APP_LOG_LEVEL_INFO, "wdw starting");
#endif
	storage_init();
	api_init();
	api_set_callback(on_park_data);

#ifdef MULTI_DESTINATION
	api_set_dest_list_callback(on_dest_list_received);
	api_set_dest_parks_callback(on_dest_parks_received);

	const char *last_dest = storage_get_last_destination();
	if (last_dest) {
		/* Returning user — fetch their destination's parks. PKJS will
		 * reply with DEST_PARKS_DATA → on_dest_parks_received populates
		 * g_parks, then either auto-loads the single park or shows the
		 * park picker. */
		loading_view_show("Loading parks...", on_startup_loading_back);
		api_select_destination(last_dest);
	} else {
		/* First launch — show the destination picker. PKJS fills in
		 * the data asynchronously via DEST_LIST_DATA. */
		dest_picker_show(on_dest_select);
		api_request_dest_list();
	}
#else
	/* Prefetch opening hours for all parks so the picker can show them
	 * on first launch, before the user has opened any individual park.
	 * The call queues if PKJS isn't ready yet; fires automatically when
	 * the ready handshake arrives. */
	api_fetch_all_hours();

	/* Auto-launch last park if known */
	const char *last = storage_get_last_park();
	int idx = -1;
	if (last) {
		for (int i = 0; i < NUM_PARKS; i++) {
			if (strcmp(g_parks[i].id, last) == 0) { idx = i; break; }
		}
	}
	if (idx >= 0) {
		load_park(idx);
	} else {
		show_picker();
	}
#endif

#ifdef TEST_MODE
	APP_LOG(APP_LOG_LEVEL_INFO, "TEST_MODE: auto-switching every %dms",
	        TEST_SWITCH_INTERVAL_MS);
	s_test_timer = app_timer_register(TEST_SWITCH_INTERVAL_MS, test_tick, NULL);
#endif
}

static void deinit(void)
{
	if (s_refresh_timer) app_timer_cancel(s_refresh_timer);
#ifdef TEST_MODE
	if (s_test_timer) app_timer_cancel(s_test_timer);
#endif
	/* Runs after app_event_loop() returns — the documented place to
	 * reload the glance so the launcher shows it once we're gone. */
	publish_glance();
	api_deinit();
}

int main(void)
{
	init();
	app_event_loop();
	deinit();
	return 0;
}
