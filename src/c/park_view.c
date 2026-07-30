/*
 * park_view.c — attractions + shows lists with a tab header.
 *
 * UI structure: Window contains a header (TextLayer for park name + hours)
 * and a MenuLayer below. Two pages: PAGE_WAITS and PAGE_SHOWS, swapped by
 * the SELECT button. The MenuLayer redraws from sorted_indices[] on every
 * data change or page switch.
 *
 * No per-row allocation: we render directly from g_park using the index
 * array. The MenuLayer asks us for one row at a time, on demand.
 */

#include "park_view.h"
#include "parkdata.h"
#include "sort.h"
#include "storage.h"
#include "config.h"
#include "colours.h"
#include "format.h"
#include <pebble.h>
#include <string.h>

/* Round displays clip the top ~8px of the rectangle — bump the header
 * height on gabbro and offset the text further down so "Magic Kingdom"
 * sits inside the visible circle. */
#define HEADER_HEIGHT PBL_IF_ROUND_ELSE(44, 36)
#define HEADER_TEXT_Y PBL_IF_ROUND_ELSE(8, 0)
#define TAB_HEIGHT 22
/* Waits rows are single-line: name + wait time. Shows rows are stacked
 * two-line (name bold on top, multiple times below) so we can fit the
 * full show name AND several upcoming times — a showtime list with only
 * the next time was regularly cut off. */
#define ROW_HEIGHT_WAITS 26
#define ROW_HEIGHT_SHOWS 42

/* Up/Down move the cursor ONE row, repeating at this interval while
 * held — the cursor is the long-press-favourite target, so it must be
 * able to land on every ride, and held-repeat covers a 40-row list in
 * ~4s. Deliberately NO double-press jump: a multi-click subscription
 * makes the firmware delay every single click by the disambiguation
 * window (~300ms), which reads as input lag (device-tested). */
#define SCROLL_REPEAT_MS 100

typedef enum { PAGE_WAITS = 0, PAGE_SHOWS = 1 } page_t;

static Window *s_window = NULL;
static Layer *s_header = NULL;
static Layer *s_tabs = NULL;
static MenuLayer *s_menu = NULL;
static char s_park_id[40];
static page_t s_page = PAGE_WAITS;
static uint8_t s_sorted[MAX_ATTRACTIONS];   /* doubles as shows index */
static uint8_t s_sorted_count = 0;
static park_view_back_cb_t s_on_back = NULL;
static park_view_settings_cb_t s_on_settings = NULL;
/* Header collapses (slides off the top) when the user scrolls down past
 * the first row, giving the attractions/shows list more vertical space.
 * Tabs stay anchored to the top of the menu area. */
static bool s_header_collapsed = false;
/* Id of the row under the cursor, tracked continuously so a background
 * refresh can put the cursor back on the same ride/show after the
 * re-sort shuffles row order (waits change → positions change). */
static char s_selected_id[MAX_ID_LEN] = "";

/* --- header ---------------------------------------------------------- */

static void header_update_proc(Layer *layer, GContext *gctx)
{
	GRect b = layer_get_bounds(layer);
	graphics_context_set_fill_color(gctx, COL_HEADER_BG);
	graphics_fill_rect(gctx, b, 0, GCornerNone);

	/* Park name (top, bold white) */
	graphics_context_set_text_color(gctx, COL_HEADER_FG);
	const char *name = g_park.park_name[0] ? g_park.park_name : "Loading...";
	graphics_draw_text(gctx, name,
	                   fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
	                   GRect(b.origin.x + 4, b.origin.y + HEADER_TEXT_Y,
	                         b.size.w - 8, 20),
	                   GTextOverflowModeTrailingEllipsis,
	                   GTextAlignmentCenter, NULL);

	/* Hours + freshness (bottom, accent tint). "· Nm" appears once data
	 * is ≥2min old so the user knows whether they're looking at live
	 * figures. Hidden when fresh to keep the header uncluttered. */
	/* Times come over the wire as 24h "HH:MM" and are rendered per the
	 * user's clock preference. When every ride is closed the line leads
	 * with "Closed" so a wall of grey rows reads as intentional. */
	char hours[48];
	char age_suffix[12];
	format_age_suffix((uint32_t)time(NULL), g_park.last_updated_secs,
	                  age_suffix, sizeof(age_suffix));
	bool closed = parkdata_all_closed();
	if (g_park.has_schedule) {
		char open[MAX_HOURS_LEN], close[MAX_HOURS_LEN];
		bool use_24h = clock_is_24h_style();
		format_clock_time(g_park.schedule_open, use_24h, open, sizeof(open));
		format_clock_time(g_park.schedule_close, use_24h, close, sizeof(close));
		snprintf(hours, sizeof(hours), "%s%s - %s%s",
		         closed ? "Closed \u00b7 " : "", open, close, age_suffix);
	} else if (closed) {
		snprintf(hours, sizeof(hours), "Closed%s", age_suffix);
	} else if (age_suffix[0]) {
		snprintf(hours, sizeof(hours), "updated%s", age_suffix);
	} else {
		hours[0] = '\0';
	}
	graphics_context_set_text_color(gctx, COL_HEADER_HOURS);
	graphics_draw_text(gctx, hours,
	                   fonts_get_system_font(FONT_KEY_GOTHIC_14),
	                   GRect(b.origin.x + 4, b.origin.y + HEADER_TEXT_Y + 18,
	                         b.size.w - 8, 18),
	                   GTextOverflowModeTrailingEllipsis,
	                   GTextAlignmentCenter, NULL);
}

/* --- menu callbacks -------------------------------------------------- */

static uint16_t menu_num_sections(MenuLayer *l, void *ctx)
{
	return 1;
}

static uint16_t menu_num_rows(MenuLayer *l, uint16_t section, void *ctx)
{
	if (s_sorted_count == 0) return 1;   /* "Empty" row */
	return s_sorted_count;
}

static int16_t menu_row_height(MenuLayer *l, MenuIndex *idx, void *ctx)
{
	return (s_page == PAGE_SHOWS) ? ROW_HEIGHT_SHOWS : ROW_HEIGHT_WAITS;
}

/* Tab strip (WAITS | SHOWS) rendered as its own Layer above the MenuLayer.
 * Keeping it outside the menu means switching pages doesn't scroll the tabs
 * off-screen — they're always visible. */
static void tabs_update_proc(Layer *layer, GContext *gctx)
{
	GRect b = layer_get_bounds(layer);
	/* A small square gear segment sits at the right end of the strip —
	 * a discoverability hint that SELECT eventually reaches settings
	 * (Waits → Shows → Settings). It's an affordance, not a button. */
	int gear_w = b.size.h;
	int half = (b.size.w - gear_w) / 2;

	bool waits_active = (s_page == PAGE_WAITS);

	/* Left: WAITS */
	graphics_context_set_fill_color(gctx,
		waits_active ? COL_TAB_ACTIVE_BG : COL_TAB_INACTIVE_BG);
	graphics_fill_rect(gctx, GRect(b.origin.x, b.origin.y, half, b.size.h),
	                   0, GCornerNone);
	graphics_context_set_text_color(gctx,
		waits_active ? COL_TAB_ACTIVE_FG : COL_TAB_INACTIVE_FG);
	graphics_draw_text(gctx, "WAITS",
		fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
		GRect(b.origin.x, b.origin.y + 2, half, b.size.h - 2),
		GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

	/* Middle: SHOWS */
	graphics_context_set_fill_color(gctx,
		waits_active ? COL_TAB_INACTIVE_BG : COL_TAB_ACTIVE_BG);
	graphics_fill_rect(gctx, GRect(b.origin.x + half, b.origin.y,
	                               b.size.w - gear_w - half, b.size.h),
	                   0, GCornerNone);
	graphics_context_set_text_color(gctx,
		waits_active ? COL_TAB_INACTIVE_FG : COL_TAB_ACTIVE_FG);
	graphics_draw_text(gctx, "SHOWS",
		fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
		GRect(b.origin.x + half, b.origin.y + 2,
		      b.size.w - gear_w - half, b.size.h - 2),
		GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

	/* Right: gear glyph. Filled circle + four teeth + a hole punched in
	 * the segment background colour. ~12px total — any finer detail is
	 * lost at this size. */
	int gx = b.origin.x + b.size.w - gear_w;
	graphics_context_set_fill_color(gctx, COL_TAB_INACTIVE_BG);
	graphics_fill_rect(gctx, GRect(gx, b.origin.y, gear_w, b.size.h),
	                   0, GCornerNone);
	int cx = gx + gear_w / 2;
	int cy = b.origin.y + b.size.h / 2;
	graphics_context_set_fill_color(gctx, COL_TAB_INACTIVE_FG);
	graphics_fill_rect(gctx, GRect(cx - 1, cy - 6, 2, 3), 0, GCornerNone);
	graphics_fill_rect(gctx, GRect(cx - 1, cy + 3, 2, 3), 0, GCornerNone);
	graphics_fill_rect(gctx, GRect(cx - 6, cy - 1, 3, 2), 0, GCornerNone);
	graphics_fill_rect(gctx, GRect(cx + 3, cy - 1, 3, 2), 0, GCornerNone);
	graphics_fill_circle(gctx, GPoint(cx, cy), 4);
	graphics_context_set_fill_color(gctx, COL_TAB_INACTIVE_BG);
	graphics_fill_circle(gctx, GPoint(cx, cy), 2);
}

static void format_wait(const attraction_t *a, char *out, int outsz)
{
	if (a->status == 'C') strncpy(out, "Closed", outsz);
	else if (a->status == 'D') strncpy(out, "Down", outsz);
	else if (a->status == 'R') strncpy(out, "Rehab", outsz);
	else if (a->wait < 0) strncpy(out, "?", outsz);
	else snprintf(out, outsz, "%dm", a->wait);
	out[outsz - 1] = '\0';
}

/* Pick the traffic-light colour for a wait. Non-operating rides and unknown
 * waits use a dim grey so they don't fight with the green/amber/red. */
static GColor wait_colour(const attraction_t *a)
{
	if (a->status != 'O' || a->wait < 0) return COL_WAIT_DIM;
	if (a->wait < 30) return COL_WAIT_GREEN;
	if (a->wait < 60) return COL_WAIT_AMBER;
	return COL_WAIT_RED;
}

/* Join all upcoming showtimes with comma separators. We print up to
 * `max_times` of them (cap < MAX_TIMES_PER_SHOW) — the dedicated shows
 * row is tall enough to display 3 comfortably on emery, slightly fewer
 * on gabbro once the circle edge is accounted for. */
static void format_show_times(const show_t *s, int max_times,
                              char *out, int outsz)
{
	out[0] = '\0';
	int limit = s->time_count;
	if (limit > max_times) limit = max_times;
	bool use_24h = clock_is_24h_style();
	for (int i = 0; i < limit; i++) {
		char t[MAX_TIME_LEN];
		format_clock_time(s->times[i], use_24h, t, sizeof(t));
		if (i > 0) strncat(out, ", ", outsz - strlen(out) - 1);
		strncat(out, t, outsz - strlen(out) - 1);
	}
}

/* Custom single-line row renderer: name (left, ellipsised) + value (right,
 * tinted by `value_col`). The highlighted row renders on the MenuLayer's
 * blue selection background with all-white text — it's the target of the
 * long-press favourite gesture, so the user needs to see where the cursor
 * is. */
/* Round-display text flow attributes — allocated lazily so we don't
 * build and tear down the GTextAttributes on every row redraw. On
 * rectangular displays this stays NULL and graphics_draw_text renders
 * straight to the rect. */
#ifdef PBL_ROUND
static GTextAttributes *s_text_attrs = NULL;
static GTextAttributes *round_attrs(void)
{
	if (!s_text_attrs) {
		s_text_attrs = graphics_text_attributes_create();
		graphics_text_attributes_enable_screen_text_flow(s_text_attrs, 4);
	}
	return s_text_attrs;
}
#endif

/* Fill the row's background with either the zebra stripe tint (odd rows)
 * or plain white (even rows). We paint this ourselves so the alternating
 * pattern shows through — MenuLayer's normal/highlight colours can only
 * set one flat background per row. Highlighted rows keep the MenuLayer's
 * selection colour (painted before we're called), so we skip the stripe
 * and return true so callers switch the text to the selected palette. */
static bool fill_row_bg(GContext *gctx, const Layer *cell, uint16_t row)
{
	if (menu_cell_layer_is_highlighted(cell)) return true;
	GRect b = layer_get_bounds(cell);
	GColor bg = (row & 1) ? COL_ROW_STRIPE : GColorWhite;
	graphics_context_set_fill_color(gctx, bg);
	graphics_fill_rect(gctx, b, 0, GCornerNone);
	return false;
}

/* Two-line renderer for Shows rows: name on top (bold 18), times on the
 * bottom (14, dim). Takes the full row width — there's no side-by-side
 * value column here. Round-display text flow is honoured on both lines. */
static void draw_row_stacked(GContext *gctx, const Layer *cell,
                             const char *name, const char *times,
                             bool fav, uint16_t row)
{
	bool selected = fill_row_bg(gctx, cell, row);

	GRect b = layer_get_bounds(cell);
	const int PAD = PBL_IF_ROUND_ELSE(10, 4);

#ifdef PBL_ROUND
	GTextAttributes *attrs = round_attrs();
#else
	GTextAttributes *attrs = NULL;
#endif

	char title[MAX_NAME_LEN + 4];
	if (fav) snprintf(title, sizeof(title), "* %s", name);
	else { strncpy(title, name, sizeof(title) - 1); title[sizeof(title) - 1] = '\0'; }

	graphics_context_set_text_color(gctx,
		selected ? COL_ROW_SELECTED_FG : COL_ROW_FG);
	graphics_draw_text(gctx, title,
		fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
		GRect(b.origin.x + PAD, b.origin.y - 2,
		      b.size.w - PAD * 2, 22),
		GTextOverflowModeTrailingEllipsis,
		GTextAlignmentLeft, attrs);

	graphics_context_set_text_color(gctx,
		selected ? COL_ROW_SELECTED_FG : COL_WAIT_DIM);
	graphics_draw_text(gctx, times,
		fonts_get_system_font(FONT_KEY_GOTHIC_14),
		GRect(b.origin.x + PAD, b.origin.y + 20,
		      b.size.w - PAD * 2, 22),
		GTextOverflowModeTrailingEllipsis,
		GTextAlignmentLeft, attrs);
}

/* Non-numeric status text ("Closed", "Rehab", "Down", "?") is drawn in a
 * smaller font so it survives the curve on gabbro's top/bottom rows where
 * text flow narrows the value rect. Wait-time numbers (which are short,
 * "65m") stay in the larger font. Detected by a leading non-digit. */
static bool is_status_text(const char *value)
{
	if (!value || !value[0]) return false;
	char c = value[0];
	return !(c >= '0' && c <= '9');
}

static void draw_row_inline(GContext *gctx, const Layer *cell,
                            const char *name, const char *value, bool fav,
                            GColor value_col, uint16_t row)
{
	bool selected = fill_row_bg(gctx, cell, row);

	GRect b = layer_get_bounds(cell);
	const int VALUE_W = 56;
	const int PAD = PBL_IF_ROUND_ELSE(10, 4);

	/* Gothic 14 for words, 18 for numbers — lets "Closed" fit inside the
	 * narrowed value column on curved rows. */
	GFont name_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
	GFont value_font = is_status_text(value)
		? fonts_get_system_font(FONT_KEY_GOTHIC_14)
		: fonts_get_system_font(FONT_KEY_GOTHIC_18);

	char title[MAX_NAME_LEN + 4];
	if (fav) snprintf(title, sizeof(title), "* %s", name);
	else { strncpy(title, name, sizeof(title) - 1); title[sizeof(title) - 1] = '\0'; }

#ifdef PBL_ROUND
	GTextAttributes *attrs = round_attrs();
#else
	GTextAttributes *attrs = NULL;
#endif

	graphics_context_set_text_color(gctx,
		selected ? COL_ROW_SELECTED_FG : COL_ROW_FG);
	graphics_draw_text(gctx, title, name_font,
		GRect(b.origin.x + PAD, b.origin.y,
		      b.size.w - VALUE_W - PAD * 2, b.size.h),
		GTextOverflowModeTrailingEllipsis,
		GTextAlignmentLeft, attrs);

	/* Gothic 14 sits ~2px higher than 18 visually; bump status y a
	 * little so it aligns with the name baseline. */
	int value_y = b.origin.y + (is_status_text(value) ? 4 : 0);
	/* Traffic-light tints are unreadable on the blue highlight — the
	 * selected row's value goes white like the rest of its text. */
	graphics_context_set_text_color(gctx,
		selected ? COL_ROW_SELECTED_FG : value_col);
	graphics_draw_text(gctx, value, value_font,
		GRect(b.origin.x + b.size.w - VALUE_W - PAD, value_y,
		      VALUE_W, b.size.h),
		GTextOverflowModeTrailingEllipsis,
		GTextAlignmentRight, attrs);
}

static void menu_draw_row(GContext *gctx, const Layer *cell,
                          MenuIndex *idx, void *ctx)
{
	if (s_sorted_count == 0) {
		const char *empty = (s_page == PAGE_WAITS) ? "No data" : "No shows today";
		draw_row_inline(gctx, cell, empty, "", false, COL_WAIT_DIM, idx->row);
		return;
	}

	uint8_t pos = s_sorted[idx->row];

	if (s_page == PAGE_WAITS) {
		const attraction_t *a = &g_park.attractions[pos];
		bool fav = storage_is_favourite(s_park_id, FAV_ATTRACTIONS, a->id);
		char wait[16];
		format_wait(a, wait, sizeof(wait));
		draw_row_inline(gctx, cell, a->name, wait, fav,
		                wait_colour(a), idx->row);
	} else {
		const show_t *sh = &g_park.shows[pos];
		bool fav = storage_is_favourite(s_park_id, FAV_SHOWS, sh->id);
		char times[48];
		/* Round displays get fewer times because the circle eats the ends
		 * of a long comma-list. Rectangular can fit 3 comfortably. */
		format_show_times(sh, PBL_IF_ROUND_ELSE(2, 3), times, sizeof(times));
		draw_row_stacked(gctx, cell, sh->name, times, fav, idx->row);
	}
}

/* --- input handling -------------------------------------------------- */

/* Collapse / expand the park-name header to free up space for attractions.
 * When collapsed, the header slides off the top of the screen and the
 * tabs + menu move up by HEADER_HEIGHT. Tabs stay visible either way. */
static void set_header_collapsed(bool collapse)
{
	if (collapse == s_header_collapsed) return;
	s_header_collapsed = collapse;
	if (!s_window) return;

	Layer *root = window_get_root_layer(s_window);
	GRect root_b = layer_get_bounds(root);
	int header_y = collapse ? -HEADER_HEIGHT : 0;
	int tabs_y = collapse ? 0 : HEADER_HEIGHT;
	int menu_y = tabs_y + TAB_HEIGHT;

	if (s_header) {
		layer_set_frame(s_header,
			GRect(0, header_y, root_b.size.w, HEADER_HEIGHT));
	}
	if (s_tabs) {
		layer_set_frame(s_tabs,
			GRect(0, tabs_y, root_b.size.w, TAB_HEIGHT));
	}
	if (s_menu) {
		layer_set_frame(menu_layer_get_layer(s_menu),
			GRect(0, menu_y, root_b.size.w, root_b.size.h - menu_y));
	}
}

static void remember_selection(void)
{
	if (!s_menu || s_sorted_count == 0) {
		s_selected_id[0] = '\0';
		return;
	}
	MenuIndex idx = menu_layer_get_selected_index(s_menu);
	if (idx.row >= s_sorted_count) {
		s_selected_id[0] = '\0';
		return;
	}
	uint8_t pos = s_sorted[idx.row];
	const char *id = (s_page == PAGE_WAITS)
		? g_park.attractions[pos].id : g_park.shows[pos].id;
	strncpy(s_selected_id, id, sizeof(s_selected_id) - 1);
	s_selected_id[sizeof(s_selected_id) - 1] = '\0';
}

static void menu_selection_changed(MenuLayer *l, MenuIndex new_idx,
                                   MenuIndex old_idx, void *ctx)
{
	/* Any row past the first triggers the collapse. Coming back to row 0
	 * restores the full header so the park name + hours are visible. */
	set_header_collapsed(new_idx.row > 0);
	remember_selection();
}

static void resort(void)
{
	if (s_page == PAGE_WAITS) {
		s_sorted_count = sort_attractions(s_park_id,
		                                  storage_get_sort_mode(), s_sorted);
	} else {
		s_sorted_count = sort_shows(s_park_id, s_sorted);
	}
	if (s_menu) menu_layer_reload_data(s_menu);
	if (s_header) layer_mark_dirty(s_header);
	if (s_tabs) layer_mark_dirty(s_tabs);

	/* Test marker — scripts/integration_tests.py looks at this line
	 * to verify that favourites really are pinned to the top of the
	 * list after toggling. */
	if (s_sorted_count > 0) {
		uint8_t top = s_sorted[0];
		const char *name = (s_page == PAGE_WAITS)
			? g_park.attractions[top].name : g_park.shows[top].name;
		const char *id = (s_page == PAGE_WAITS)
			? g_park.attractions[top].id : g_park.shows[top].id;
		bool is_fav = storage_is_favourite(s_park_id,
			s_page == PAGE_WAITS ? FAV_ATTRACTIONS : FAV_SHOWS, id);
		APP_LOG(APP_LOG_LEVEL_INFO, "PV top %s%s",
			name, is_fav ? " FAV" : "");
	}
}

static void switch_page(page_t p)
{
	if (s_page == p) return;
	s_page = p;
	resort();
	if (s_menu) {
		menu_layer_set_selected_index(s_menu,
		                              (MenuIndex){.section = 0, .row = 0},
		                              MenuRowAlignTop, false);
	}
	/* Back to row 0 — re-show the header. */
	set_header_collapsed(false);
	remember_selection();
}

/* Move the selection by `delta` rows, clamped to the list bounds.
 * No animation: the eased scroll reads as input lag on a one-row hop,
 * especially while the button repeats. */
static void jump_rows(int delta)
{
	if (!s_menu || s_sorted_count == 0) return;
	MenuIndex cur = menu_layer_get_selected_index(s_menu);
	int target = (int)cur.row + delta;
	if (target < 0) target = 0;
	if (target >= s_sorted_count) target = s_sorted_count - 1;
	MenuIndex next = { .section = 0, .row = (uint16_t)target };
	menu_layer_set_selected_index(s_menu, next, MenuRowAlignCenter, false);
}

static void up_handler(ClickRecognizerRef ref, void *ctx)
{
	jump_rows(-1);
}

static void down_handler(ClickRecognizerRef ref, void *ctx)
{
	jump_rows(1);
}

static void select_handler(ClickRecognizerRef ref, void *ctx)
{
	if (s_page == PAGE_WAITS) {
		switch_page(PAGE_SHOWS);
	} else {
		if (s_on_settings) s_on_settings();
	}
}

/* Long-press SELECT: toggle favourite on the highlighted row, in place.
 * This is the fast path for the app's most common personalisation action
 * — the settings round-trip (Shows → Settings → Fave list → find ride)
 * still works but is no longer required. The short vibe confirms the
 * toggle; the animated re-sort shows the row jumping to (or leaving)
 * the pinned favourites bucket, with the cursor following it. */
static void long_select_handler(ClickRecognizerRef ref, void *ctx)
{
	if (!s_menu || s_sorted_count == 0) return;
	MenuIndex idx = menu_layer_get_selected_index(s_menu);
	if (idx.row >= s_sorted_count) return;
	uint8_t pos = s_sorted[idx.row];
	const char *id = (s_page == PAGE_WAITS)
		? g_park.attractions[pos].id : g_park.shows[pos].id;
	fav_list_kind_t kind = (s_page == PAGE_WAITS)
		? FAV_ATTRACTIONS : FAV_SHOWS;

	storage_toggle_favourite(s_park_id, kind, id);
	vibes_short_pulse();
	/* Same marker format as settings_view.c — integration tests grep it. */
	APP_LOG(APP_LOG_LEVEL_INFO, "SV toggle %s %s", id,
	        storage_is_favourite(s_park_id, kind, id) ? "ON" : "OFF");

	resort();
	int row = sort_find_id(s_sorted, s_sorted_count, kind, id);
	if (row < 0) row = 0;
	menu_layer_set_selected_index(s_menu,
	                              (MenuIndex){.section = 0, .row = (uint16_t)row},
	                              MenuRowAlignCenter, true);
	set_header_collapsed(row > 0);
	remember_selection();
}

static void back_handler(ClickRecognizerRef ref, void *ctx)
{
	if (s_page == PAGE_SHOWS) {
		switch_page(PAGE_WAITS);
	} else {
		if (s_on_back) s_on_back();
	}
}

static void click_config(Window *w)
{
	/* Handle all 4 buttons ourselves. Up/Down scroll the menu manually.
	 * With a long-click subscription in place, the plain SELECT click
	 * fires on release (before the 500ms threshold) — page switching
	 * keeps working unchanged. */
	window_single_repeating_click_subscribe(BUTTON_ID_UP, SCROLL_REPEAT_MS, up_handler);
	window_single_repeating_click_subscribe(BUTTON_ID_DOWN, SCROLL_REPEAT_MS, down_handler);
	window_single_click_subscribe(BUTTON_ID_SELECT, select_handler);
	window_long_click_subscribe(BUTTON_ID_SELECT, 500, long_select_handler, NULL);
	window_single_click_subscribe(BUTTON_ID_BACK, back_handler);
}

/* --- window lifecycle ------------------------------------------------ */

/* Tick the header once a minute so the "· 3m" staleness suffix updates
 * without waiting for the next data refresh. */
static void on_minute_tick(struct tm *tick_time, TimeUnits units_changed)
{
	if (s_header) layer_mark_dirty(s_header);
}

/* Called every time the park view becomes visible — including when we pop
 * back from the settings window. We re-sort here so any sort-mode or
 * favourites change made in settings is reflected immediately. */
static void window_appear(Window *w)
{
	resort();
	/* Rows may have moved (favourites toggled in settings) — re-sync the
	 * remembered cursor id with whatever the cursor now sits on. */
	remember_selection();
	tick_timer_service_subscribe(MINUTE_UNIT, on_minute_tick);
}

static void window_disappear(Window *w)
{
	tick_timer_service_unsubscribe();
}

static void window_load(Window *w)
{
	Layer *root = window_get_root_layer(w);
	GRect bounds = layer_get_bounds(root);

	/* Header: park name + hours (dark-blue themed). */
	s_header = layer_create(GRect(0, 0, bounds.size.w, HEADER_HEIGHT));
	layer_set_update_proc(s_header, header_update_proc);
	layer_add_child(root, s_header);

	/* Tabs: WAITS | SHOWS. Always visible — not inside the menu header. */
	s_tabs = layer_create(GRect(0, HEADER_HEIGHT,
	                            bounds.size.w, TAB_HEIGHT));
	layer_set_update_proc(s_tabs, tabs_update_proc);
	layer_add_child(root, s_tabs);

	/* Menu: rows. The cursor row is visibly highlighted — it's the
	 * target of the long-press favourite gesture. */
	s_menu = menu_layer_create(GRect(0, HEADER_HEIGHT + TAB_HEIGHT,
	                                 bounds.size.w,
	                                 bounds.size.h - HEADER_HEIGHT - TAB_HEIGHT));
	MenuLayerCallbacks cbs = {
		.get_num_sections = menu_num_sections,
		.get_num_rows = menu_num_rows,
		.get_cell_height = menu_row_height,
		.draw_row = menu_draw_row,
		.selection_changed = menu_selection_changed,
	};
	menu_layer_set_callbacks(s_menu, NULL, cbs);
#ifdef PBL_COLOR
	menu_layer_set_highlight_colors(s_menu,
		COL_ROW_SELECTED_BG, COL_ROW_SELECTED_FG);
	menu_layer_set_normal_colors(s_menu, GColorWhite, COL_ROW_FG);
#endif
#ifdef PBL_ROUND
	/* Default on round is centre-focused (selected row stays mid-screen).
	 * We turn it off so the list starts at the top of its viewport like
	 * the rectangular layout — there's no per-row selection in the park
	 * view anyway, just scrolling. */
	menu_layer_set_center_focused(s_menu, false);
#endif
	layer_add_child(root, menu_layer_get_layer(s_menu));

	window_set_click_config_provider(w, (ClickConfigProvider)click_config);
	resort();
}

static void window_unload(Window *w)
{
	if (s_menu) { menu_layer_destroy(s_menu); s_menu = NULL; }
	if (s_tabs) { layer_destroy(s_tabs); s_tabs = NULL; }
	if (s_header) { layer_destroy(s_header); s_header = NULL; }
}

void park_view_show(const char *park_id,
                    park_view_back_cb_t on_back,
                    park_view_settings_cb_t on_settings)
{
	if (park_id) {
		strncpy(s_park_id, park_id, sizeof(s_park_id) - 1);
		s_park_id[sizeof(s_park_id) - 1] = '\0';
	}
	s_on_back = on_back;
	s_on_settings = on_settings;
	/* Don't reset s_page here — returning from settings should land on the
	 * same page we left from. park_view_data_changed() resets it when a
	 * fresh park load happens. */
	if (!s_window) {
		s_window = window_create();
		window_set_window_handlers(s_window, (WindowHandlers){
			.load = window_load,
			.appear = window_appear,
			.disappear = window_disappear,
			.unload = window_unload,
		});
	}
	if (!window_stack_contains_window(s_window)) {
		window_stack_push(s_window, true);
	}
}

void park_view_hide(void)
{
	if (s_window) window_stack_remove(s_window, false);
}

void park_view_data_changed(void)
{
	/* Fresh park data — always land the user on the Waits page with the
	 * header expanded so they see the park name and hours again. */
	s_page = PAGE_WAITS;
	resort();
	if (s_menu) {
		menu_layer_set_selected_index(s_menu,
		                              (MenuIndex){.section = 0, .row = 0},
		                              MenuRowAlignTop, false);
	}
	set_header_collapsed(false);
	remember_selection();
}

void park_view_refresh(void)
{
	/* Background refresh — g_park was just re-parsed. Unlike
	 * park_view_data_changed(), the user keeps their place: same page,
	 * cursor back on the same ride/show (found by id, since the re-sort
	 * can move every row as waits change). No window-stack changes, so
	 * this is safe to call while settings sits on top. */
	MenuIndex before = s_menu ? menu_layer_get_selected_index(s_menu)
	                          : (MenuIndex){ .section = 0, .row = 0 };
	resort();
	if (!s_menu) return;

	int target = sort_find_id(s_sorted, s_sorted_count,
	                          (s_page == PAGE_WAITS) ? FAV_ATTRACTIONS : FAV_SHOWS,
	                          s_selected_id);
	if (target < 0) {
		/* Row vanished (e.g. show ran out of times) — stay near where the
		 * cursor was, clamped into the new list. */
		target = before.row;
		if (s_sorted_count == 0) target = 0;
		else if (target >= s_sorted_count) target = s_sorted_count - 1;
	}
	menu_layer_set_selected_index(s_menu,
	                              (MenuIndex){.section = 0, .row = (uint16_t)target},
	                              MenuRowAlignCenter, false);
	set_header_collapsed(target > 0);
	remember_selection();
	APP_LOG(APP_LOG_LEVEL_INFO, "PV refresh kept row %d", target);
}
