/*
 * park_picker.c — list of parks shown at launch (or when user backs out
 * of the park view).
 *
 * Visual treatment: the same dark-blue header as the park view ("PICK A
 * PARK"), 44-px rows with a large bold title, and the same blue
 * highlight on the selected row. Custom row rendering matches the look
 * of the attractions/shows page so the app feels cohesive.
 */

#include "park_picker.h"
#include "config.h"
#include "colours.h"
#include "storage.h"
#include <pebble.h>
#include <string.h>
#include <stdio.h>

/* Round (gabbro) needs a taller header — the top of the circle clips
 * several pixels so we push the "Walt Disney World" label down to where
 * the viewport is fully visible. */
#define HEADER_HEIGHT PBL_IF_ROUND_ELSE(46, 32)
/* Taller rows now because each shows park name on top + hours below. */
#define ROW_HEIGHT 44
/* Credits footer at the bottom of the window. Fixed-height layer.
 * Skipped entirely on round: at 260×260 gabbro the bottom ~20px fall
 * outside the visible circle, so the footer text would be clipped to
 * unreadable. On round we rely on the corresponding entry in the
 * settings view for the attribution. */
#define FOOTER_HEIGHT PBL_IF_ROUND_ELSE(0, 14)

static Window *s_window = NULL;
static Layer *s_header = NULL;
static MenuLayer *s_menu = NULL;
static Layer *s_footer = NULL;
static park_picker_select_cb_t s_callback = NULL;

#ifdef PBL_ROUND
static GTextAttributes *s_round_attrs = NULL;
#endif

static void header_update_proc(Layer *layer, GContext *gctx)
{
	GRect b = layer_get_bounds(layer);
	graphics_context_set_fill_color(gctx, COL_HEADER_BG);
	graphics_fill_rect(gctx, b, 0, GCornerNone);
	graphics_context_set_text_color(gctx, COL_HEADER_FG);
	/* On round, the top ~14px of the header rect fall outside the visible
	 * circle, so offset the text down to stay within it. */
	const int TEXT_Y = PBL_IF_ROUND_ELSE(16, 2);
	graphics_draw_text(gctx, APP_NAME,
		fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
		GRect(b.origin.x + 4, b.origin.y + TEXT_Y,
		      b.size.w - 8, b.size.h - TEXT_Y),
		GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

/* Map a colour token (from destinations/<name>.json's per-park `color`
 * field) to a Pebble GColor. Unknown tokens render as dark grey so a
 * typo doesn't crash the app. On black-and-white platforms we fold
 * everything down to black. */
static GColor colour_from_token(const char *token)
{
#ifdef PBL_COLOR
	if (!token) return GColorDarkGray;
	if (!strcmp(token, "pink"))   return GColorShockingPink;
	if (!strcmp(token, "cyan"))   return GColorVividCerulean;
	if (!strcmp(token, "blue"))   return GColorBlueMoon;
	if (!strcmp(token, "red"))    return GColorRed;
	if (!strcmp(token, "green"))  return GColorIslamicGreen;
	if (!strcmp(token, "orange")) return GColorOrange;
	if (!strcmp(token, "yellow")) return GColorYellow;
	if (!strcmp(token, "purple")) return GColorPurple;
	return GColorDarkGray;
#else
	(void)token;
	return GColorBlack;
#endif
}

/* ── Icon shapes ─────────────────────────────────────────────────────
 * Each icon function draws its shape centred on (cx, cy) in the park's
 * theme colour, within an ~14x14 box. Keep them simple enough to read
 * at small size — no fine detail survives on the Pebble display.
 */

/* Castle silhouette — wide base wall with three battlements and a
 * central tall spire. */
static void draw_castle(GContext *gctx, int cx, int cy, GColor col)
{
	graphics_context_set_fill_color(gctx, col);
	graphics_fill_rect(gctx, GRect(cx - 6, cy,     12, 5), 0, GCornerNone);
	graphics_fill_rect(gctx, GRect(cx - 6, cy - 3, 3,  3), 0, GCornerNone);
	graphics_fill_rect(gctx, GRect(cx - 2, cy - 5, 4,  5), 0, GCornerNone);
	graphics_fill_rect(gctx, GRect(cx + 3, cy - 3, 3,  3), 0, GCornerNone);
	graphics_fill_rect(gctx, GRect(cx - 1, cy - 7, 2,  2), 0, GCornerNone);
}

/* Sphere — just the circle. An earlier iteration drew a white equator
 * line across it; at 12px diameter that bisected the icon and read as
 * "cut in half" rather than "geodesic dome", so we dropped it. */
static void draw_sphere(GContext *gctx, int cx, int cy, GColor col)
{
	graphics_context_set_fill_color(gctx, col);
	graphics_fill_circle(gctx, GPoint(cx, cy - 1), 6);
}

/* Five-point star. Outer radius 7, inner radius 3, vertices at 36°
 * intervals from straight up. Coordinates come from the exact polar
 * math — (R·sinθ, −R·cosθ) for outer, (r·sinθ, −r·cosθ) for inner —
 * rounded to the nearest integer pixel. Two earlier attempts flattened
 * the bottom by forcing the lower inner vertices onto the same y as
 * the upper ones; that read as "stubby legs". Keeping each inner
 * vertex at its true rounded y gives a proper pentagram silhouette. */
static const GPathInfo STAR_PATH_INFO = {
	.num_points = 10,
	.points = (GPoint[]) {
		{  0, -7 },   /* outer:   0° (top) */
		{  2, -2 },   /* inner:  36° */
		{  7, -2 },   /* outer:  72° (upper-right) */
		{  3,  1 },   /* inner: 108° */
		{  4,  6 },   /* outer: 144° (lower-right) */
		{  0,  3 },   /* inner: 180° (bottom) */
		{ -4,  6 },   /* outer: 216° (lower-left) */
		{ -3,  1 },   /* inner: 252° */
		{ -7, -2 },   /* outer: 288° (upper-left) */
		{ -2, -2 },   /* inner: 324° */
	},
};

static void draw_star(GContext *gctx, int cx, int cy, GColor col)
{
	GPath *p = gpath_create(&STAR_PATH_INFO);
	gpath_move_to(p, GPoint(cx, cy));
	graphics_context_set_fill_color(gctx, col);
	gpath_draw_filled(gctx, p);
	gpath_destroy(p);
}

/* Tree — proper triangular canopy drawn as a filled GPath, with a
 * short trunk rectangle below. Earlier "stack of rectangles" gave a
 * pixellated pyramid; the triangle reads cleaner at this size. */
static const GPathInfo TREE_CANOPY_INFO = {
	.num_points = 3,
	.points = (GPoint[]) {
		{  0, -7 },   /* apex */
		{  6,  3 },   /* bottom-right */
		{ -6,  3 },   /* bottom-left */
	},
};

static void draw_tree(GContext *gctx, int cx, int cy, GColor col)
{
	graphics_context_set_fill_color(gctx, col);
	GPath *p = gpath_create(&TREE_CANOPY_INFO);
	gpath_move_to(p, GPoint(cx, cy));
	gpath_draw_filled(gctx, p);
	gpath_destroy(p);
	/* Trunk — same colour so it reads as one icon. */
	graphics_fill_rect(gctx, GRect(cx - 1, cy + 3, 2, 4), 0, GCornerNone);
}

/* Dispatch to the right shape based on the park's icon token. Unknown
 * tokens fall back to a coloured circle so new icons in the JSON
 * fail-soft instead of crashing. */
static void draw_park_icon(GContext *gctx, int cx, int cy, uint16_t row)
{
	const char *icon = g_parks[row].icon;
	GColor col = colour_from_token(g_parks[row].color);
	if (icon && !strcmp(icon, "castle"))      draw_castle(gctx, cx, cy, col);
	else if (icon && !strcmp(icon, "sphere")) draw_sphere(gctx, cx, cy, col);
	else if (icon && !strcmp(icon, "star"))   draw_star(gctx, cx, cy, col);
	else if (icon && !strcmp(icon, "tree"))   draw_tree(gctx, cx, cy, col);
	else {
		graphics_context_set_fill_color(gctx, col);
		graphics_fill_circle(gctx, GPoint(cx, cy), 5);
	}
}

#ifndef PBL_ROUND
/* Footer: tiny credit line at the bottom of the picker window.
 * Rect-only — on round the footer area is outside the visible circle
 * so we don't render it at all (and skip this function to keep the
 * compiler happy about unused statics). */
static void footer_update_proc(Layer *layer, GContext *gctx)
{
	GRect b = layer_get_bounds(layer);
	graphics_context_set_fill_color(gctx, GColorWhite);
	graphics_fill_rect(gctx, b, 0, GCornerNone);
	graphics_context_set_text_color(gctx, COL_WAIT_DIM);
	graphics_draw_text(gctx, "Powered by ThemeParks.wiki",
		fonts_get_system_font(FONT_KEY_GOTHIC_14),
		GRect(b.origin.x + 2, b.origin.y - 2, b.size.w - 4, b.size.h),
		GTextOverflowModeTrailingEllipsis,
		GTextAlignmentCenter, NULL);
}
#endif

static uint16_t menu_get_num_rows(MenuLayer *l, uint16_t section, void *ctx)
{
	return NUM_PARKS;
}

static int16_t menu_get_cell_height(MenuLayer *l, MenuIndex *idx, void *ctx)
{
	return ROW_HEIGHT;
}

static void menu_draw_row(GContext *gctx, const Layer *cell,
                          MenuIndex *idx, void *ctx)
{
	GRect b = layer_get_bounds(cell);
	bool selected = menu_cell_layer_is_highlighted(cell);

	/* Selected rows are drawn by the MenuLayer with highlight colours; for
	 * non-selected rows we paint the zebra stripe ourselves. */
	if (!selected) {
		GColor bg = (idx->row & 1) ? COL_ROW_STRIPE : GColorWhite;
		graphics_context_set_fill_color(gctx, bg);
		graphics_fill_rect(gctx, b, 0, GCornerNone);
	}

	/* Round: PAD is driven by the circle-intersection at the last
	 * row. With 4 rows of 44px under a 46px header, the bottom row
	 * runs y=178..222. At y=222 the circle's half-width is only
	 * ~92px, so the visible x-range at that scanline is x∈[38,222];
	 * anything to the left of x=38 clips. PAD=40 keeps the icon's
	 * left edge at x=40, comfortably inside — and because the icon
	 * is only ever drawn in its row's vertical slice, the tighter
	 * circle inset near the bottom is what matters. Rect keeps the
	 * original tight PAD. */
	const int PAD = PBL_IF_ROUND_ELSE(40, 8);
	const int ICON_BOX = 14;   /* each icon is drawn in a ~14px square */
	const int ICON_GAP = 6;    /* gap between icon and name */
	const int TEXT_X = PAD + ICON_BOX + ICON_GAP;

#ifdef PBL_ROUND
	if (!s_round_attrs) {
		s_round_attrs = graphics_text_attributes_create();
		graphics_text_attributes_enable_screen_text_flow(s_round_attrs, 4);
	}
	GTextAttributes *attrs = s_round_attrs;
#else
	GTextAttributes *attrs = NULL;
#endif

	/* Park icon — distinct per park (castle / sphere / star / tree),
	 * drawn in the park's theme colour and centred in the leftmost
	 * column of the row. */
	draw_park_icon(gctx,
		b.origin.x + PAD + ICON_BOX / 2,
		b.origin.y + 16,
		idx->row);

	/* Name (top, offset past the icon) */
	graphics_context_set_text_color(gctx, selected ? COL_ROW_SELECTED_FG : COL_ROW_FG);
	graphics_draw_text(gctx, g_parks[idx->row].name,
		fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
		GRect(b.origin.x + TEXT_X, b.origin.y + 2,
		      b.size.w - TEXT_X - PAD, 22),
		GTextOverflowModeTrailingEllipsis,
		GTextAlignmentLeft, attrs);

	/* Hours (bottom, small + dim) — shown only once we've cached them
	 * (either from a previous visit or the startup prefetch). Indented
	 * to align under the name, not the icon. */
	char open[PARK_HOURS_STR_LEN], close[PARK_HOURS_STR_LEN];
	if (storage_get_park_hours(g_parks[idx->row].id, open, close)) {
		char hours[32];
		snprintf(hours, sizeof(hours), "%s - %s", open, close);
		GColor hours_col = selected
			? COL_ROW_SELECTED_FG : COL_WAIT_DIM;
		graphics_context_set_text_color(gctx, hours_col);
		graphics_draw_text(gctx, hours,
			fonts_get_system_font(FONT_KEY_GOTHIC_14),
			GRect(b.origin.x + TEXT_X, b.origin.y + 22,
			      b.size.w - TEXT_X - PAD, 20),
			GTextOverflowModeTrailingEllipsis,
			GTextAlignmentLeft, attrs);
	}
}

static void menu_select(MenuLayer *l, MenuIndex *idx, void *ctx)
{
	if (s_callback) s_callback(idx->row);
}

static void window_load(Window *w)
{
	Layer *root = window_get_root_layer(w);
	GRect bounds = layer_get_bounds(root);

	s_header = layer_create(GRect(0, 0, bounds.size.w, HEADER_HEIGHT));
	layer_set_update_proc(s_header, header_update_proc);
	layer_add_child(root, s_header);

	s_menu = menu_layer_create(GRect(0, HEADER_HEIGHT, bounds.size.w,
	                                 bounds.size.h - HEADER_HEIGHT
	                                               - FOOTER_HEIGHT));
	menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
		.get_num_rows = menu_get_num_rows,
		.draw_row = menu_draw_row,
		.get_cell_height = menu_get_cell_height,
		.select_click = menu_select,
	});
#ifdef PBL_COLOR
	menu_layer_set_highlight_colors(s_menu,
		COL_ROW_SELECTED_BG, COL_ROW_SELECTED_FG);
	menu_layer_set_normal_colors(s_menu, GColorWhite, COL_ROW_FG);
#endif
#ifdef PBL_ROUND
	/* On round, menu_layer defaults to center_focused=true — which
	 * renders the selected row at the middle with lots of blank space
	 * around it. We want all 4 parks visible at once, with PAD large
	 * enough to keep icons inside the circle's bottom-row inset, so
	 * force it off. */
	menu_layer_set_center_focused(s_menu, false);
#endif
	menu_layer_set_click_config_onto_window(s_menu, w);
	layer_add_child(root, menu_layer_get_layer(s_menu));

#ifndef PBL_ROUND
	/* Footer — small attribution line at the bottom of the window.
	 * Rectangular platforms only; on round the footer area is outside
	 * the visible circle. */
	s_footer = layer_create(GRect(0, bounds.size.h - FOOTER_HEIGHT,
	                              bounds.size.w, FOOTER_HEIGHT));
	layer_set_update_proc(s_footer, footer_update_proc);
	layer_add_child(root, s_footer);
#endif
}

static void window_unload(Window *w)
{
	if (s_menu) { menu_layer_destroy(s_menu); s_menu = NULL; }
	if (s_footer) { layer_destroy(s_footer); s_footer = NULL; }
	if (s_header) { layer_destroy(s_header); s_header = NULL; }
}

void park_picker_show(park_picker_select_cb_t on_select)
{
	s_callback = on_select;
	if (!s_window) {
		s_window = window_create();
		window_set_window_handlers(s_window, (WindowHandlers){
			.load = window_load,
			.unload = window_unload,
		});
	}
	window_stack_push(s_window, true);
}

void park_picker_hide(void)
{
	if (s_window) window_stack_remove(s_window, false);
}
