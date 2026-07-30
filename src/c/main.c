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
#endif
#include <pebble.h>
#include <string.h>

static int s_current_park = -1;
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
	 * again (relaunch auto-resumes the same destination). */
	dest_picker_show(on_dest_select);
	if (!dest_picker_has_data()) {
		api_request_dest_list();
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
	/* Parse "id|name|icon|color\n..." into g_parks[] */
	num_parks = 0;
	uint16_t pos = 0;
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

/* The composed glance line lives here because subtitle_template_string
 * is read when the reload session commits, after prime_glance returns —
 * a stack buffer would be gone by then. */
static char s_glance_buf[64];
static time_t s_glance_expiry;

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

/* Today's close as an epoch — the hours slice's expiration, so a stale
 * glance never outlives its truth. */
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
	/* An empty buffer means the ladder produced nothing — adding no
	 * slice here is what clears any previous glance back to the plain
	 * app name. */
	if (limit < 1 || s_glance_buf[0] == '\0') return;
	const AppGlanceSlice slice = {
		.layout = {
			.icon = APP_GLANCE_SLICE_DEFAULT_ICON,
			.subtitle_template_string = s_glance_buf,
		},
		.expiration_time = s_glance_expiry,
	};
	AppGlanceResult res = app_glance_add_slice(session, slice);
	if (res != APP_GLANCE_RESULT_SUCCESS) {
		APP_LOG(APP_LOG_LEVEL_WARNING, "glance: add_slice failed (%d)",
		        (int)res);
	}
}

/* Build the glance line for the last-viewed park and hand it to the
 * launcher. Called from deinit(), i.e. every app exit. */
static void publish_glance(void)
{
	s_glance_buf[0] = '\0';
	s_glance_expiry = 0;

	time_t now = time(NULL);
	struct tm lt = *localtime(&now);
	int now_min = lt.tm_hour * 60 + lt.tm_min;

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
		glance_input_t in = {
			.park_name = g_parks[idx].name,
			.wait_mins = -1,
		};

		/* Hours: prefer the live fetch (g_park only holds this park's
		 * data when it's still current), else the persisted cache —
		 * which works offline and when the fetch never completed. */
		char open_buf[PARK_HOURS_STR_LEN], close_buf[PARK_HOURS_STR_LEN];
		const char *open_s = NULL, *close_s = NULL;
		if (idx == s_current_park && g_park.has_schedule) {
			open_s = g_park.schedule_open;
			close_s = g_park.schedule_close;
		} else if (storage_get_park_hours(park_id, open_buf, close_buf)) {
			open_s = open_buf;
			close_s = close_buf;
		}
		time_t close_epoch = 0;
		if (open_s && close_s) {
			int open_min = glance_hhmm_to_min(open_s);
			int close_min = glance_hhmm_to_min(close_s);
			/* Any parse failure (legacy cached strings) → hours are not
			 * "current"; the ladder falls through to the wait rung. */
			if (open_min >= 0 && close_min >= 0
			    && format_hours_current(now_min, open_min, close_min)) {
				close_epoch = glance_close_epoch(now, now_min,
				                                 open_min, close_min);
				if (close_epoch > now) {
					in.has_schedule = true;
					in.hours_current = true;
					in.open_hhmm = open_s;
					in.close_hhmm = close_s;
				}
			}
		}

		/* Headline wait: only from live data for this park, and only
		 * fresh enough that the launcher won't show a fossil. */
		if (idx == s_current_park && g_park.last_updated_secs != 0
		    && now >= (time_t)g_park.last_updated_secs) {
			uint32_t age = (uint32_t)now - g_park.last_updated_secs;
			if (age < GLANCE_WAIT_MAX_AGE_SECS) {
				const attraction_t *a = glance_headline(park_id);
				if (a) {
					in.ride_name = a->name;
					in.wait_mins = a->wait;
					in.wait_age_secs = age;
				}
			}
		}

		int rung = format_glance_line(&in, clock_is_24h_style(),
		                              s_glance_buf, sizeof(s_glance_buf));
		if (rung == GLANCE_RUNG_HOURS) s_glance_expiry = close_epoch;
		else if (rung == GLANCE_RUNG_WAIT)
			s_glance_expiry = now + GLANCE_WAIT_MAX_AGE_SECS;
		else s_glance_buf[0] = '\0';
	}

	APP_LOG(APP_LOG_LEVEL_INFO, "glance: %s",
	        s_glance_buf[0] ? s_glance_buf : "none");
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
