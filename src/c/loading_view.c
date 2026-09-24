/*
 * loading_view.c — shown while fetching park data, reused as an error
 * screen when the fetch fails or times out.
 */

#include "loading_view.h"
#include "colours.h"
#include "ui_common.h"
#include <pebble.h>
#include <string.h>

static Window *s_window = NULL;
/* The error states can sit on screen for a while ("Phone not
 * connected"), so this window keeps the clock too. */
static StatusBarLayer *s_status = NULL;
static TextLayer *s_label = NULL;
static char s_text[128];

static loading_retry_cb_t s_on_retry = NULL;
static loading_back_cb_t s_on_back = NULL;
static bool s_is_error = false;

/* Animated trailing dots while loading ("Loading\nMagic Kingdom", then
 * ".", "..", "..." cycling). Cheap reassurance that the app hasn't hung
 * — the fetch watchdog handles the case where it actually has. Error
 * screens are static: their text is an instruction, not a wait. */
#define DOTS_INTERVAL_MS 400
static char s_base_text[112];
static AppTimer *s_dots_timer = NULL;
static int s_dots = 0;

static void dots_tick(void *ctx)
{
	s_dots = (s_dots % 3) + 1;
	snprintf(s_text, sizeof(s_text), "%s%.*s", s_base_text, s_dots, "...");
	if (s_label) text_layer_set_text(s_label, s_text);
	s_dots_timer = app_timer_register(DOTS_INTERVAL_MS, dots_tick, NULL);
}

static void dots_stop(void)
{
	if (s_dots_timer) {
		app_timer_cancel(s_dots_timer);
		s_dots_timer = NULL;
	}
}

static void dots_start(void)
{
	dots_stop();
	s_dots = 3;   /* start fully-dotted so the text doesn't jump */
	snprintf(s_text, sizeof(s_text), "%s...", s_base_text);
	s_dots_timer = app_timer_register(DOTS_INTERVAL_MS, dots_tick, NULL);
}

static void select_handler(ClickRecognizerRef ref, void *ctx)
{
	if (s_is_error && s_on_retry) s_on_retry();
}

static void back_handler(ClickRecognizerRef ref, void *ctx)
{
	if (s_on_back) s_on_back();
}

static void click_config(Window *w)
{
	window_single_click_subscribe(BUTTON_ID_SELECT, select_handler);
	window_single_click_subscribe(BUTTON_ID_BACK, back_handler);
}

static void window_load(Window *w)
{
	Layer *root = window_get_root_layer(w);
	GRect b = layer_get_bounds(root);

#ifdef PBL_COLOR
	window_set_background_color(w, GColorWhite);
#endif

	s_label = text_layer_create(GRect(8, b.size.h / 2 - 40,
	                                  b.size.w - 16, 90));
	text_layer_set_text(s_label, s_text);
	text_layer_set_text_alignment(s_label, GTextAlignmentCenter);
	text_layer_set_font(s_label, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
	text_layer_set_background_color(s_label, GColorClear);
#ifdef PBL_COLOR
	text_layer_set_text_color(s_label, COL_HEADER_BG);
#endif
	layer_add_child(root, text_layer_get_layer(s_label));
	s_status = ui_status_bar_create(root);

	window_set_click_config_provider(w, (ClickConfigProvider)click_config);
}

static void window_unload(Window *w)
{
	if (s_status) { status_bar_layer_destroy(s_status); s_status = NULL; }
	if (s_label) { text_layer_destroy(s_label); s_label = NULL; }
}

static void ensure_window(void)
{
	if (!s_window) {
		s_window = window_create();
		window_set_window_handlers(s_window, (WindowHandlers){
			.load = window_load, .unload = window_unload,
		});
	}
	if (!window_stack_contains_window(s_window)) {
		window_stack_push(s_window, true);
	}
}

void loading_view_show(const char *park_name, loading_back_cb_t on_back)
{
	snprintf(s_base_text, sizeof(s_base_text), "Loading\n%s",
	         park_name ? park_name : "");
	s_is_error = false;
	s_on_retry = NULL;
	s_on_back = on_back;
	ensure_window();
	dots_start();
	if (s_label) text_layer_set_text(s_label, s_text);
}

void loading_view_show_error(const char *park_name,
                             loading_retry_cb_t on_retry,
                             loading_back_cb_t on_back)
{
	dots_stop();
	snprintf(s_text, sizeof(s_text),
	         "Couldn't load\n%s\n\nSELECT to retry\nBACK for parks",
	         park_name ? park_name : "this park");
	s_is_error = true;
	s_on_retry = on_retry;
	s_on_back = on_back;
	ensure_window();
	if (s_label) text_layer_set_text(s_label, s_text);
}

void loading_view_show_disconnected(loading_retry_cb_t on_retry,
                                    loading_back_cb_t on_back)
{
	dots_stop();
	snprintf(s_text, sizeof(s_text),
	         "Phone not\nconnected\n\nSELECT to retry\nBACK for parks");
	s_is_error = true;
	s_on_retry = on_retry;
	s_on_back = on_back;
	ensure_window();
	if (s_label) text_layer_set_text(s_label, s_text);
}

void loading_view_hide(void)
{
	dots_stop();
	if (s_window) window_stack_remove(s_window, false);
}
