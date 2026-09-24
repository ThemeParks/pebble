/*
 * settings_view.c — three sub-views: main menu, sort menu, favourites lists.
 *
 * State machine driven by s_sub. Back button pops sub-views back to main,
 * then back again exits to the park view.
 */

#include "settings_view.h"
#include "parkdata.h"
#include "storage.h"
#include "api.h"
#include "config.h"
#include "colours.h"
#include "ui_common.h"
#include <pebble.h>
#include <string.h>

typedef enum {
	SUB_MAIN = 0,
	SUB_SORT = 1,
	SUB_FAV_ATTR = 2,
	SUB_FAV_SHOWS = 3,
} sub_t;

/* Status bar (time) + title strip; the list starts below both. */
#define LIST_Y (UI_STATUS_H + UI_TITLE_H)

static Window *s_window = NULL;
static StatusBarLayer *s_status = NULL;
static Layer *s_header = NULL;
static MenuLayer *s_menu = NULL;
static char s_park_id[40];
static sub_t s_sub = SUB_MAIN;
static settings_back_cb_t s_on_back = NULL;
static settings_refresh_cb_t s_on_refresh = NULL;

/* Index arrays for fav lists (alphabetical by name). */
static uint8_t s_fav_idx[MAX_ATTRACTIONS];
static uint8_t s_fav_count = 0;

/* "Refresh Data" is an action, not a setting — it lives last so the
 * everyday personalisation rows keep their muscle-memory positions. */
static const char *MAIN_LABELS[] = { "Sort", "Fave Attractions", "Fave Shows",
                                     "Refresh Data" };
static const char *SORT_LABELS[] = { "Longest wait", "Shortest wait", "A - Z" };

static int alpha_compare_attr(uint8_t ai, uint8_t bi)
{
	return strcmp(g_park.attractions[ai].name, g_park.attractions[bi].name);
}

static int alpha_compare_show(uint8_t ai, uint8_t bi)
{
	return strcmp(g_park.shows[ai].name, g_park.shows[bi].name);
}

static void sort_alpha_attr(uint8_t *arr, uint8_t n)
{
	for (uint8_t i = 1; i < n; i++) {
		uint8_t key = arr[i];
		int j = i - 1;
		while (j >= 0 && alpha_compare_attr(arr[j], key) > 0) {
			arr[j + 1] = arr[j]; j--;
		}
		arr[j + 1] = key;
	}
}

static void sort_alpha_show(uint8_t *arr, uint8_t n)
{
	for (uint8_t i = 1; i < n; i++) {
		uint8_t key = arr[i];
		int j = i - 1;
		while (j >= 0 && alpha_compare_show(arr[j], key) > 0) {
			arr[j + 1] = arr[j]; j--;
		}
		arr[j + 1] = key;
	}
}

static void rebuild_fav_indices(void)
{
	if (s_sub == SUB_FAV_ATTR) {
		s_fav_count = g_park.attraction_count;
		for (uint8_t i = 0; i < s_fav_count; i++) s_fav_idx[i] = i;
		sort_alpha_attr(s_fav_idx, s_fav_count);
	} else if (s_sub == SUB_FAV_SHOWS) {
		s_fav_count = g_park.show_count;
		for (uint8_t i = 0; i < s_fav_count; i++) s_fav_idx[i] = i;
		sort_alpha_show(s_fav_idx, s_fav_count);
	}
}

/* Header: shows the current sub-view's title (SETTINGS / SORT BY / FAVES). */
static const char *sub_title(void)
{
	switch (s_sub) {
	case SUB_SORT: return "SORT BY";
	case SUB_FAV_ATTR: return "FAVE RIDES";
	case SUB_FAV_SHOWS: return "FAVE SHOWS";
	default: return "SETTINGS";
	}
}

static void header_update_proc(Layer *layer, GContext *gctx)
{
	GRect b = layer_get_bounds(layer);
	graphics_context_set_fill_color(gctx, COL_HEADER_BG);
	graphics_fill_rect(gctx, b, 0, GCornerNone);
	graphics_context_set_text_color(gctx, COL_HEADER_FG);
	graphics_draw_text(gctx, sub_title(),
		fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
		GRect(b.origin.x + 4, b.origin.y + UI_TITLE_TEXT_Y,
		      b.size.w - 8, b.size.h - UI_TITLE_TEXT_Y),
		GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

/* --- menu callbacks -------------------------------------------------- */

static uint16_t menu_num_rows(MenuLayer *l, uint16_t section, void *ctx)
{
	switch (s_sub) {
	case SUB_MAIN: return 4;
	case SUB_SORT: return 3;
	case SUB_FAV_ATTR: return s_fav_count;
	case SUB_FAV_SHOWS: return s_fav_count;
	}
	return 0;
}

static int16_t menu_row_height(MenuLayer *l, MenuIndex *idx, void *ctx)
{
	return 30;
}

/* Round-display text flow attrs — allocated once, used per row. */
#ifdef PBL_ROUND
static GTextAttributes *s_round_attrs = NULL;
static GTextAttributes *round_attrs(void)
{
	if (!s_round_attrs) {
		s_round_attrs = graphics_text_attributes_create();
		graphics_text_attributes_enable_screen_text_flow(s_round_attrs, 4);
	}
	return s_round_attrs;
}
#endif

/* Custom row renderer: zebra stripe, bold text, selected highlight, and
 * an asterisk prefix for chosen-sort / favourite items. Matches the
 * look of the park picker. */
static void draw_settings_row(GContext *gctx, const Layer *cell,
                              uint16_t row, const char *text, bool marked)
{
	GRect b = layer_get_bounds(cell);
	bool selected = menu_cell_layer_is_highlighted(cell);
	if (!selected) {
		GColor bg = (row & 1) ? COL_ROW_STRIPE : GColorWhite;
		graphics_context_set_fill_color(gctx, bg);
		graphics_fill_rect(gctx, b, 0, GCornerNone);
	}

	const int PAD = PBL_IF_ROUND_ELSE(10, 8);
#ifdef PBL_ROUND
	GTextAttributes *attrs = round_attrs();
#else
	GTextAttributes *attrs = NULL;
#endif

	char buf[MAX_NAME_LEN + 4];
	snprintf(buf, sizeof(buf), "%s%s", marked ? "* " : "  ", text);

	graphics_context_set_text_color(gctx,
		selected ? COL_ROW_SELECTED_FG : COL_ROW_FG);
	graphics_draw_text(gctx, buf,
		fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
		GRect(b.origin.x + PAD, b.origin.y + 2,
		      b.size.w - PAD * 2, b.size.h - 2),
		GTextOverflowModeTrailingEllipsis,
		GTextAlignmentLeft, attrs);
}

static void menu_draw_row(GContext *gctx, const Layer *cell,
                          MenuIndex *idx, void *ctx)
{
	switch (s_sub) {
	case SUB_MAIN:
		draw_settings_row(gctx, cell, idx->row,
			MAIN_LABELS[idx->row], false);
		break;
	case SUB_SORT: {
		sort_mode_t cur = storage_get_sort_mode();
		draw_settings_row(gctx, cell, idx->row,
			SORT_LABELS[idx->row], idx->row == (int)cur);
		break;
	}
	case SUB_FAV_ATTR: {
		uint8_t pos = s_fav_idx[idx->row];
		const attraction_t *a = &g_park.attractions[pos];
		bool fav = storage_is_favourite(s_park_id, FAV_ATTRACTIONS, a->id);
		draw_settings_row(gctx, cell, idx->row, a->name, fav);
		break;
	}
	case SUB_FAV_SHOWS: {
		uint8_t pos = s_fav_idx[idx->row];
		const show_t *s = &g_park.shows[pos];
		bool fav = storage_is_favourite(s_park_id, FAV_SHOWS, s->id);
		draw_settings_row(gctx, cell, idx->row, s->name, fav);
		break;
	}
	}
}

static void enter_sub(sub_t new_sub)
{
	s_sub = new_sub;
	if (new_sub == SUB_FAV_ATTR || new_sub == SUB_FAV_SHOWS) {
		rebuild_fav_indices();
	}
	if (s_menu) {
		menu_layer_set_selected_index(s_menu,
		                              (MenuIndex){.section = 0, .row = 0},
		                              MenuRowAlignTop, false);
		menu_layer_reload_data(s_menu);
	}
	if (s_header) layer_mark_dirty(s_header);
}

static void menu_select(MenuLayer *l, MenuIndex *idx, void *ctx)
{
	switch (s_sub) {
	case SUB_MAIN:
		if (idx->row == 0) enter_sub(SUB_SORT);
		else if (idx->row == 1) enter_sub(SUB_FAV_ATTR);
		else if (idx->row == 2) enter_sub(SUB_FAV_SHOWS);
		else if (idx->row == 3) {
			/* Manual refresh: hand control back to main.c, which
			 * closes settings and reloads the park bypassing the
			 * phone-side cache. */
			APP_LOG(APP_LOG_LEVEL_INFO, "SV manual refresh");
			if (s_on_refresh) s_on_refresh();
		}
		break;
	case SUB_SORT:
		storage_set_sort_mode((sort_mode_t)idx->row);
		enter_sub(SUB_MAIN);
		break;
	case SUB_FAV_ATTR: {
		uint8_t pos = s_fav_idx[idx->row];
		const attraction_t *a = &g_park.attractions[pos];
		storage_toggle_favourite(s_park_id, FAV_ATTRACTIONS, a->id);
		/* Log marker used by scripts/integration_tests.py to detect
		 * whether the net state ended ON or OFF across multiple toggles. */
		APP_LOG(APP_LOG_LEVEL_INFO, "SV toggle %s %s",
			a->id,
			storage_is_favourite(s_park_id, FAV_ATTRACTIONS, a->id) ? "ON" : "OFF");
		/* Mirror the new state to the phone (timeline pins / ride
		 * alerts read it from localStorage there). */
		api_send_favs_sync(s_park_id);
		menu_layer_reload_data(s_menu);
		break;
	}
	case SUB_FAV_SHOWS: {
		uint8_t pos = s_fav_idx[idx->row];
		const show_t *sh = &g_park.shows[pos];
		storage_toggle_favourite(s_park_id, FAV_SHOWS, sh->id);
		APP_LOG(APP_LOG_LEVEL_INFO, "SV toggle %s %s",
			sh->id,
			storage_is_favourite(s_park_id, FAV_SHOWS, sh->id) ? "ON" : "OFF");
		api_send_favs_sync(s_park_id);
		menu_layer_reload_data(s_menu);
		break;
	}
	}
}

static void back_handler(ClickRecognizerRef ref, void *ctx)
{
	if (s_sub != SUB_MAIN) {
		enter_sub(SUB_MAIN);
	} else {
		if (s_on_back) s_on_back();
	}
}

/* UP/DOWN: one row per press, repeating while held (ui_common.h). The
 * old double-tap letter jump is gone on purpose — it hijacked two quick
 * presses (meaning "two rows") into a bucket jump, and it blocked
 * hold-to-repeat, which is the faster way through a 40-entry list. */
static void up_handler(ClickRecognizerRef ref, void *ctx)
{
	if (!s_menu) return;
	menu_layer_set_selected_next(s_menu, true, MenuRowAlignCenter, true);
}

static void down_handler(ClickRecognizerRef ref, void *ctx)
{
	if (!s_menu) return;
	menu_layer_set_selected_next(s_menu, false, MenuRowAlignCenter, true);
}

static void select_click_handler(ClickRecognizerRef ref, void *ctx)
{
	if (!s_menu) return;
	MenuIndex idx = menu_layer_get_selected_index(s_menu);
	menu_select(s_menu, &idx, NULL);
}

static void click_config(Window *w)
{
	window_single_repeating_click_subscribe(BUTTON_ID_UP, SCROLL_REPEAT_MS, up_handler);
	window_single_repeating_click_subscribe(BUTTON_ID_DOWN, SCROLL_REPEAT_MS, down_handler);
	window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
	window_single_click_subscribe(BUTTON_ID_BACK, back_handler);
}

/* --- window lifecycle ------------------------------------------------ */

static void window_load(Window *w)
{
	Layer *root = window_get_root_layer(w);
	GRect bounds = layer_get_bounds(root);

	s_header = layer_create(GRect(0, UI_STATUS_H, bounds.size.w, UI_TITLE_H));
	layer_set_update_proc(s_header, header_update_proc);
	layer_add_child(root, s_header);

	s_menu = menu_layer_create(GRect(0, LIST_Y,
	                                 bounds.size.w,
	                                 bounds.size.h - LIST_Y));
	menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
		.get_num_rows = menu_num_rows,
		.get_cell_height = menu_row_height,
		.draw_row = menu_draw_row,
		.select_click = menu_select,
	});
#ifdef PBL_COLOR
	menu_layer_set_highlight_colors(s_menu,
		COL_ROW_SELECTED_BG, COL_ROW_SELECTED_FG);
	menu_layer_set_normal_colors(s_menu, GColorWhite, COL_ROW_FG);
#endif
#ifdef PBL_ROUND
	/* Top-aligned list — centre-focus pushes our 3-row main menu down to
	 * the middle of the screen which leaves a huge gap below the header. */
	menu_layer_set_center_focused(s_menu, false);
#endif
	layer_add_child(root, menu_layer_get_layer(s_menu));
	s_status = ui_status_bar_create(root);
	window_set_click_config_provider(w, (ClickConfigProvider)click_config);
}

static void window_unload(Window *w)
{
	if (s_status) { status_bar_layer_destroy(s_status); s_status = NULL; }
	if (s_menu) { menu_layer_destroy(s_menu); s_menu = NULL; }
	if (s_header) { layer_destroy(s_header); s_header = NULL; }
}

void settings_view_show(const char *park_id, settings_back_cb_t on_back,
                        settings_refresh_cb_t on_refresh)
{
	if (park_id) {
		strncpy(s_park_id, park_id, sizeof(s_park_id) - 1);
		s_park_id[sizeof(s_park_id) - 1] = '\0';
	}
	s_on_back = on_back;
	s_on_refresh = on_refresh;
	s_sub = SUB_MAIN;
	if (!s_window) {
		s_window = window_create();
		window_set_window_handlers(s_window, (WindowHandlers){
			.load = window_load,
			.unload = window_unload,
		});
	}
	if (!window_stack_contains_window(s_window)) {
		window_stack_push(s_window, true);
	}
}

void settings_view_hide(void)
{
	if (s_window) window_stack_remove(s_window, false);
}
