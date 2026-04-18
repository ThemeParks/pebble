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
#define FETCH_WATCHDOG_MS 20000   /* 20s — beyond the api.c retry window */

#ifdef TEST_MODE
static AppTimer *s_test_timer = NULL;
static int s_test_switch_count = 0;
#define TEST_SWITCH_INTERVAL_MS 2000
#endif

static void on_park_data(bool ok);
static void load_park(int park_idx);
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

static void on_error_back(void)
{
	show_picker();
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
	park_view_hide();
	settings_view_hide();
	loading_view_hide();
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

static void show_settings(void)
{
	if (s_current_park < 0) return;
	settings_view_show(g_parks[s_current_park].id, on_settings_back);
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

	/* PKJS pushes hours unprompted after DEST_PARKS_DATA (via its
	 * handleDestSelect .then(fetchAllHours) chain). We don't need to
	 * call api_fetch_all_hours here — and doing so races with the
	 * outbox when we immediately load_park below. */

	if (num_parks == 1) {
		load_park(0);
	} else if (num_parks > 0) {
		dest_picker_hide();
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
	loading_view_show("Loading parks...");
	api_select_destination(dest_id);
}

#endif

/* --- park loading --------------------------------------------------- */

static void on_park_data(bool ok)
{
	cancel_fetch_watchdog();
	if (!ok) {
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
	show_park();
	park_view_data_changed();
	schedule_refresh();
}

static void load_park(int park_idx)
{
	if (park_idx < 0 || park_idx >= NUM_PARKS) return;
	s_current_park = park_idx;
	storage_set_last_park(g_parks[park_idx].id);

	/* Clear stale data so the loading screen header is empty */
	parkdata_clear();
	parkdata_set_park_name(g_parks[park_idx].name);

	loading_view_show(g_parks[park_idx].name);
	schedule_fetch_watchdog();
	api_fetch_park(g_parks[park_idx].id);
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
		loading_view_show("Loading parks...");
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
	api_deinit();
}

int main(void)
{
	init();
	app_event_loop();
	deinit();
	return 0;
}
