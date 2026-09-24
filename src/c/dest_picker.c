/*
 * dest_picker.c — browse all ThemeParks.wiki destinations by proximity
 * and region. Uber-app only — per-destination builds skip this screen.
 *
 * Wire format from PKJS (pipe-delimited, newline-separated):
 *   H|Section Name          — section header (Nearby, Europe, etc.)
 *   D|id|name|parkCount|distKm   — destination row
 *
 * Two-level UI over that data:
 *   Main window:  "Nearby" rows (up to 10, GPS permitting) followed by
 *                 one row per region. Selecting a region pushes the
 *                 region window; selecting a nearby row picks it.
 *   Region window: that region's destinations grouped into letter
 *                 sections. UP/DOWN scroll (repeating while held);
 *                 SELECT picks.
 */

#include "config.h"

#ifdef MULTI_DESTINATION

#include "dest_picker.h"
#include "dest_parse.h"
#include "colours.h"
#include "format.h"
#include "ui_common.h"
#include <pebble.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Destination data + parser live in dest_parse.{c,h} (host-testable);
 * this file is purely the two-window UI over that store. */

static Window *s_window = NULL;
static StatusBarLayer *s_status = NULL;
static Layer *s_header = NULL;
static MenuLayer *s_menu = NULL;
static dest_picker_select_cb_t s_callback = NULL;

/* --- region sub-window state ----------------------------------------- */

/* One letter bucket per A–Z plus '#' for anything else. PKJS sends each
 * region's destinations already alphabetically sorted, so buckets are
 * contiguous runs. */
#define MAX_LETTERS 27

static Window *s_region_window = NULL;
static StatusBarLayer *s_region_status = NULL;
static Layer *s_region_header = NULL;
static MenuLayer *s_region_menu = NULL;
static int s_region_section = -1;            /* g_dest_sections index being browsed */
static char s_letter[MAX_LETTERS];
static int s_letter_start[MAX_LETTERS];      /* offset within the region */
static int s_letter_count[MAX_LETTERS];
static int s_num_letters = 0;

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

/* Status bar (time) + title strip; the list starts below both. */
#define LIST_Y (UI_STATUS_H + UI_TITLE_H)
#define ROW_HEIGHT 38

static void rebuild_region_index(void);

/* --- region helpers -------------------------------------------------- */

/* Number of region (non-Nearby) sections. */
static int region_count(void)
{
    return g_dest_nearby_section >= 0 ? g_dest_section_count - 1 : g_dest_section_count;
}

/* Map a 0-based region row to its g_dest_sections index, skipping Nearby. */
static int region_to_section(int region_row)
{
    int r = 0;
    for (int s = 0; s < g_dest_section_count; s++) {
        if (s == g_dest_nearby_section) continue;
        if (r == region_row) return s;
        r++;
    }
    return -1;
}

static char letter_bucket(const char *name)
{
    char c = name[0];
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < 'A' || c > 'Z') c = '#';
    return c;
}

/* Build the letter index for s_region_section. Destinations within a
 * section arrive alphabetically sorted from PKJS, so equal-letter runs
 * are contiguous. */
static void rebuild_region_index(void)
{
    s_num_letters = 0;
    if (s_region_section < 0 || s_region_section >= g_dest_section_count) return;
    dest_section_t *sec = &g_dest_sections[s_region_section];
    for (int i = 0; i < sec->count; i++) {
        char c = letter_bucket(g_dests[sec->first_row + i].name);
        if (s_num_letters > 0 && s_letter[s_num_letters - 1] == c) {
            s_letter_count[s_num_letters - 1]++;
        } else if (s_num_letters < MAX_LETTERS) {
            s_letter[s_num_letters] = c;
            s_letter_start[s_num_letters] = i;
            s_letter_count[s_num_letters] = 1;
            s_num_letters++;
        } else {
            /* Out of buckets (can't happen with 27) — fold into last. */
            s_letter_count[s_num_letters - 1]++;
        }
    }
}

static dest_entry_t *region_dest_at(MenuIndex *idx)
{
    if (s_region_section < 0 || idx->section >= s_num_letters) return NULL;
    int i = g_dest_sections[s_region_section].first_row
            + s_letter_start[idx->section] + idx->row;
    if (i < 0 || i >= g_dest_count) return NULL;
    return &g_dests[i];
}

/* --- shared rendering ------------------------------------------------ */

/* Draw one destination row: bold name on top, dim subtitle underneath
 * (distance when known, park count otherwise). `stripe_row` drives the
 * zebra striping on rectangular displays. */
static void draw_dest_row(GContext *gctx, const Layer *cell,
                          const dest_entry_t *d, int stripe_row, bool selected)
{
    GRect b = layer_get_bounds(cell);

    /* Background (zebra stripe when not selected — MenuLayer paints
     * the highlight for selected rows via the colors set in window_load).
     * On round, skip the stripe: a full-width fill clips hard against
     * the circle mask and visually overrides any curve-following the
     * text does. Plain white lets the curved display read naturally. */
#ifndef PBL_ROUND
    if (!selected) {
        GColor bg = (stripe_row & 1) ? COL_ROW_STRIPE : GColorWhite;
        graphics_context_set_fill_color(gctx, bg);
        graphics_fill_rect(gctx, b, 0, GCornerNone);
    }
#endif

    /* Round: PAD=40 matches park_picker.c — that's the circle-intersection
     * at the bottom row for a 260×260 display, so the text rect sits
     * fully inside the visible circle at every row, and
     * screen_text_flow insets a further 4px per line for graceful
     * curve-following. Rect: minimal padding. */
    const int PAD = PBL_IF_ROUND_ELSE(40, 6);
#ifdef PBL_ROUND
    GTextAttributes *attrs = round_attrs();
    GTextAlignment align = GTextAlignmentCenter;
#else
    GTextAttributes *attrs = NULL;
    GTextAlignment align = GTextAlignmentLeft;
#endif

    /* Destination name */
    graphics_context_set_text_color(gctx, selected ? COL_ROW_SELECTED_FG : COL_ROW_FG);
    graphics_draw_text(gctx, d->name,
        fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
        GRect(b.origin.x + PAD, b.origin.y + 1, b.size.w - PAD * 2, 22),
        GTextOverflowModeTrailingEllipsis,
        align, attrs);

    /* Subtitle: distance if we have it, otherwise park count. Unit
     * follows the watch locale — US/UK users read miles, everyone else
     * kilometres. Decided once; locale doesn't change mid-session. */
    static bool s_units_decided = false;
    static bool s_use_miles = false;
    if (!s_units_decided) {
        s_use_miles = format_locale_uses_miles(i18n_get_system_locale());
        s_units_decided = true;
    }
    char subtitle[24];
    if (d->dist_km > 0) {
        format_distance(d->dist_km, s_use_miles, subtitle, sizeof(subtitle));
    } else {
        snprintf(subtitle, sizeof(subtitle), "%d park%s",
                 d->park_count, d->park_count == 1 ? "" : "s");
    }
    GColor sub_col = selected ? COL_ROW_SELECTED_FG : COL_WAIT_DIM;
    graphics_context_set_text_color(gctx, sub_col);
    graphics_draw_text(gctx, subtitle,
        fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(b.origin.x + PAD, b.origin.y + 18, b.size.w - PAD * 2, 16),
        GTextOverflowModeTrailingEllipsis,
        align, attrs);
}

static void draw_window_header(GContext *gctx, Layer *layer, const char *title)
{
    GRect b = layer_get_bounds(layer);
    graphics_context_set_fill_color(gctx, COL_HEADER_BG);
    graphics_fill_rect(gctx, b, 0, GCornerNone);
    graphics_context_set_text_color(gctx, COL_HEADER_FG);
    graphics_draw_text(gctx, title,
        fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
        GRect(b.origin.x + 4, b.origin.y + UI_TITLE_TEXT_Y,
              b.size.w - 8, b.size.h - UI_TITLE_TEXT_Y),
        GTextOverflowModeTrailingEllipsis,
        GTextAlignmentCenter, NULL);
}

static void draw_section_header(GContext *gctx, const Layer *cell, const char *title)
{
    GRect b = layer_get_bounds(cell);
    graphics_context_set_fill_color(gctx, GColorLightGray);
    graphics_fill_rect(gctx, b, 0, GCornerNone);
    graphics_context_set_text_color(gctx, GColorBlack);
    /* Round: centre the section label inside the visible circle and use
     * text-flow so it doesn't clip on the left edge. Rect: flush-left. */
#ifdef PBL_ROUND
    graphics_draw_text(gctx, title,
        fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
        GRect(b.origin.x + 4, b.origin.y - 1, b.size.w - 8, b.size.h),
        GTextOverflowModeTrailingEllipsis,
        GTextAlignmentCenter, round_attrs());
#else
    graphics_draw_text(gctx, title,
        fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
        GRect(b.origin.x + 6, b.origin.y - 1, b.size.w - 12, b.size.h),
        GTextOverflowModeTrailingEllipsis,
        GTextAlignmentLeft, NULL);
#endif
}

/* --- main header ----------------------------------------------------- */

static void header_update_proc(Layer *layer, GContext *gctx)
{
    /* This window lists destinations (resorts), not parks — the park
     * picker comes after choosing one. */
    draw_window_header(gctx, layer, "Pick a Destination");
}

/* --- main menu callbacks ---------------------------------------------
 * Section 0: Nearby destinations (absent without GPS).
 * Last section: one row per region.
 */

static bool has_nearby(void)
{
    return g_dest_nearby_section >= 0 && g_dest_sections[g_dest_nearby_section].count > 0;
}

/* When PKJS reports no location fix we still render a "Nearby" section
 * with a single dim hint row — a silently missing section would read
 * as "the app is different today" rather than "check the phone's GPS". */
static bool show_no_location_hint(void)
{
    return g_dest_no_location && !has_nearby();
}

static bool is_region_menu_section(uint16_t section)
{
    return !((has_nearby() || show_no_location_hint()) && section == 0);
}

static uint16_t menu_get_num_sections(struct MenuLayer *l, void *ctx)
{
    if (g_dest_section_count == 0) return 1;
    return (has_nearby() || show_no_location_hint()) ? 2 : 1;
}

static uint16_t menu_get_num_rows(struct MenuLayer *l, uint16_t section, void *ctx)
{
    if (g_dest_section_count == 0) return 1;  /* "Loading..." placeholder */
    if (!is_region_menu_section(section)) {
        return has_nearby() ? g_dest_sections[g_dest_nearby_section].count : 1;
    }
    return region_count();
}

static int16_t menu_get_header_height(struct MenuLayer *l, uint16_t section, void *ctx)
{
    if (g_dest_section_count == 0) return 0;
    return 18;
}

static void menu_draw_header(GContext *gctx, const Layer *cell,
                             uint16_t section, void *ctx)
{
    if (g_dest_section_count == 0) return;
    draw_section_header(gctx, cell,
        is_region_menu_section(section) ? "Regions" : "Nearby");
}

static int16_t menu_get_cell_height(struct MenuLayer *l, MenuIndex *idx, void *ctx)
{
    return ROW_HEIGHT;
}

static dest_entry_t *nearby_dest_at(MenuIndex *idx)
{
    if (!has_nearby()) return NULL;
    int i = g_dest_sections[g_dest_nearby_section].first_row + idx->row;
    if (i < 0 || i >= g_dest_count) return NULL;
    return &g_dests[i];
}

static void menu_draw_row(GContext *gctx, const Layer *cell,
                          MenuIndex *idx, void *ctx)
{
    GRect b = layer_get_bounds(cell);
    bool selected = menu_cell_layer_is_highlighted(cell);

    if (g_dest_count == 0) {
        /* Placeholder row when no data yet */
        graphics_context_set_text_color(gctx, GColorBlack);
        graphics_draw_text(gctx, "Loading destinations...",
            fonts_get_system_font(FONT_KEY_GOTHIC_18),
            GRect(b.origin.x + 8, b.origin.y + 8, b.size.w - 16, 24),
            GTextOverflowModeTrailingEllipsis,
            GTextAlignmentCenter, NULL);
        return;
    }

    if (!is_region_menu_section(idx->section)) {
        if (show_no_location_hint()) {
            /* Dim, non-selectable hint in place of nearby rows. */
            const int PAD_H = PBL_IF_ROUND_ELSE(40, 6);
            graphics_context_set_text_color(gctx,
                selected ? COL_ROW_SELECTED_FG : COL_ROW_FG);
            graphics_draw_text(gctx, "No location",
                fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                GRect(b.origin.x + PAD_H, b.origin.y + 1,
                      b.size.w - PAD_H * 2, 22),
                GTextOverflowModeTrailingEllipsis,
                PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft),
                NULL);
            graphics_context_set_text_color(gctx,
                selected ? COL_ROW_SELECTED_FG : COL_WAIT_DIM);
            graphics_draw_text(gctx, "Check phone GPS access",
                fonts_get_system_font(FONT_KEY_GOTHIC_14),
                GRect(b.origin.x + PAD_H, b.origin.y + 18,
                      b.size.w - PAD_H * 2, 16),
                GTextOverflowModeTrailingEllipsis,
                PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft),
                NULL);
            return;
        }
        dest_entry_t *d = nearby_dest_at(idx);
        if (d) draw_dest_row(gctx, cell, d, idx->row, selected);
        return;
    }

    /* Region row: region name + destination count. */
    int sec = region_to_section(idx->row);
    if (sec < 0) return;

#ifndef PBL_ROUND
    if (!selected) {
        GColor bg = (idx->row & 1) ? COL_ROW_STRIPE : GColorWhite;
        graphics_context_set_fill_color(gctx, bg);
        graphics_fill_rect(gctx, b, 0, GCornerNone);
    }
#endif
    const int PAD = PBL_IF_ROUND_ELSE(40, 6);
#ifdef PBL_ROUND
    GTextAttributes *attrs = round_attrs();
    GTextAlignment align = GTextAlignmentCenter;
#else
    GTextAttributes *attrs = NULL;
    GTextAlignment align = GTextAlignmentLeft;
#endif
    graphics_context_set_text_color(gctx, selected ? COL_ROW_SELECTED_FG : COL_ROW_FG);
    graphics_draw_text(gctx, g_dest_sections[sec].name,
        fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
        GRect(b.origin.x + PAD, b.origin.y + 1, b.size.w - PAD * 2, 22),
        GTextOverflowModeTrailingEllipsis, align, attrs);

    char subtitle[28];
    snprintf(subtitle, sizeof(subtitle), "All %d destination%s",
             g_dest_sections[sec].count,
             g_dest_sections[sec].count == 1 ? "" : "s");
    graphics_context_set_text_color(gctx,
        selected ? COL_ROW_SELECTED_FG : COL_WAIT_DIM);
    graphics_draw_text(gctx, subtitle,
        fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(b.origin.x + PAD, b.origin.y + 18, b.size.w - PAD * 2, 16),
        GTextOverflowModeTrailingEllipsis, align, attrs);
}

static void region_window_push(int section_idx);

static void menu_select(struct MenuLayer *l, MenuIndex *idx, void *ctx)
{
    if (g_dest_count == 0) return;
    if (!is_region_menu_section(idx->section)) {
        if (show_no_location_hint()) return;   /* hint row — not a pick */
        dest_entry_t *d = nearby_dest_at(idx);
        if (!d || !s_callback) return;
        APP_LOG(APP_LOG_LEVEL_INFO, "dest_picker: selected %s", d->name);
        s_callback(d->id, d->park_count);
        return;
    }
    int sec = region_to_section(idx->row);
    if (sec < 0) return;
    APP_LOG(APP_LOG_LEVEL_INFO, "dest_picker: browse region %s",
            g_dest_sections[sec].name);
    region_window_push(sec);
}

/* --- region window --------------------------------------------------- */

static void region_header_update_proc(Layer *layer, GContext *gctx)
{
    const char *title = (s_region_section >= 0 && s_region_section < g_dest_section_count)
        ? g_dest_sections[s_region_section].name : "";
    draw_window_header(gctx, layer, title);
}

static uint16_t region_get_num_sections(struct MenuLayer *l, void *ctx)
{
    return s_num_letters > 0 ? s_num_letters : 1;
}

static uint16_t region_get_num_rows(struct MenuLayer *l, uint16_t section, void *ctx)
{
    if (section >= s_num_letters) return 0;
    return s_letter_count[section];
}

static int16_t region_get_header_height(struct MenuLayer *l, uint16_t section, void *ctx)
{
    return 16;
}

static void region_draw_header(GContext *gctx, const Layer *cell,
                               uint16_t section, void *ctx)
{
    if (section >= s_num_letters) return;
    char title[2] = { s_letter[section], '\0' };
    draw_section_header(gctx, cell, title);
}

static void region_draw_row(GContext *gctx, const Layer *cell,
                            MenuIndex *idx, void *ctx)
{
    dest_entry_t *d = region_dest_at(idx);
    if (!d) return;
    draw_dest_row(gctx, cell, d, idx->row,
                  menu_cell_layer_is_highlighted(cell));
}

/* Click config: single UP/DOWN scrolls one row, repeating while held —
 * no letter-jump gesture. A multi-click subscription would make the
 * firmware delay every single click by its ~300ms disambiguation
 * window (see ui_common.h); the letter SECTION HEADERS stay, giving
 * fast-scrolling users their place in the alphabet. */

static void region_scroll_click(ClickRecognizerRef rec, void *ctx)
{
    bool up = click_recognizer_get_button_id(rec) == BUTTON_ID_UP;
    menu_layer_set_selected_next(s_region_menu, up,
                                 MenuRowAlignCenter, true);
}

static void region_select_click(ClickRecognizerRef rec, void *ctx)
{
    MenuIndex idx = menu_layer_get_selected_index(s_region_menu);
    dest_entry_t *d = region_dest_at(&idx);
    if (!d || !s_callback) return;
    APP_LOG(APP_LOG_LEVEL_INFO, "dest_picker: selected %s", d->name);
    s_callback(d->id, d->park_count);
}

static void region_back_click(ClickRecognizerRef rec, void *ctx)
{
    window_stack_pop(true);
}

static void region_click_config(void *ctx)
{
    window_single_repeating_click_subscribe(BUTTON_ID_UP, SCROLL_REPEAT_MS, region_scroll_click);
    window_single_repeating_click_subscribe(BUTTON_ID_DOWN, SCROLL_REPEAT_MS, region_scroll_click);
    window_single_click_subscribe(BUTTON_ID_SELECT, region_select_click);
    window_single_click_subscribe(BUTTON_ID_BACK, region_back_click);
}

static void region_window_load(Window *w)
{
    Layer *root = window_get_root_layer(w);
    GRect bounds = layer_get_bounds(root);

    s_region_header = layer_create(GRect(0, UI_STATUS_H, bounds.size.w, UI_TITLE_H));
    layer_set_update_proc(s_region_header, region_header_update_proc);
    layer_add_child(root, s_region_header);

    s_region_menu = menu_layer_create(GRect(0, LIST_Y, bounds.size.w,
                                            bounds.size.h - LIST_Y));
    menu_layer_set_callbacks(s_region_menu, NULL, (MenuLayerCallbacks){
        .get_num_sections = region_get_num_sections,
        .get_num_rows = region_get_num_rows,
        .get_header_height = region_get_header_height,
        .draw_header = region_draw_header,
        .draw_row = region_draw_row,
        .get_cell_height = menu_get_cell_height,
    });
#ifdef PBL_COLOR
    menu_layer_set_highlight_colors(s_region_menu,
        COL_ROW_SELECTED_BG, COL_ROW_SELECTED_FG);
    menu_layer_set_normal_colors(s_region_menu, GColorWhite, COL_ROW_FG);
#endif
#ifdef PBL_ROUND
    menu_layer_set_center_focused(s_region_menu, false);
#endif
    /* NOT menu_layer_set_click_config_onto_window — we need our own
     * provider for the double-press letter jump. */
    window_set_click_config_provider(w, region_click_config);
    layer_add_child(root, menu_layer_get_layer(s_region_menu));
    s_region_status = ui_status_bar_create(root);
}

static void region_window_unload(Window *w)
{
    if (s_region_status) { status_bar_layer_destroy(s_region_status); s_region_status = NULL; }
    if (s_region_menu) { menu_layer_destroy(s_region_menu); s_region_menu = NULL; }
    if (s_region_header) { layer_destroy(s_region_header); s_region_header = NULL; }
}

static void region_window_push(int section_idx)
{
    s_region_section = section_idx;
    rebuild_region_index();
    if (!s_region_window) {
        s_region_window = window_create();
        window_set_window_handlers(s_region_window, (WindowHandlers){
            .load = region_window_load,
            .unload = region_window_unload,
        });
    }
    if (!window_stack_contains_window(s_region_window)) {
        window_stack_push(s_region_window, true);
    }
    if (s_region_menu) {
        menu_layer_reload_data(s_region_menu);
        menu_layer_set_selected_index(s_region_menu,
            (MenuIndex){ .section = 0, .row = 0 }, MenuRowAlignTop, false);
    }
    if (s_region_header) layer_mark_dirty(s_region_header);
}

/* --- main window lifecycle ------------------------------------------- */

static void window_load(Window *w)
{
    Layer *root = window_get_root_layer(w);
    GRect bounds = layer_get_bounds(root);

    s_header = layer_create(GRect(0, UI_STATUS_H, bounds.size.w, UI_TITLE_H));
    layer_set_update_proc(s_header, header_update_proc);
    layer_add_child(root, s_header);

    s_menu = menu_layer_create(GRect(0, LIST_Y, bounds.size.w,
                                     bounds.size.h - LIST_Y));
    menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
        .get_num_sections = menu_get_num_sections,
        .get_num_rows = menu_get_num_rows,
        .get_header_height = menu_get_header_height,
        .draw_header = menu_draw_header,
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
    menu_layer_set_center_focused(s_menu, false);
#endif
    menu_layer_set_click_config_onto_window(s_menu, w);
    layer_add_child(root, menu_layer_get_layer(s_menu));
    s_status = ui_status_bar_create(root);
}

static void window_unload(Window *w)
{
    if (s_status) { status_bar_layer_destroy(s_status); s_status = NULL; }
    if (s_menu) { menu_layer_destroy(s_menu); s_menu = NULL; }
    if (s_header) { layer_destroy(s_header); s_header = NULL; }
}

/* --- public API ----------------------------------------------------- */

void dest_picker_show(dest_picker_select_cb_t on_select)
{
    s_callback = on_select;
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

void dest_picker_hide(void)
{
    if (s_region_window) window_stack_remove(s_region_window, false);
    if (s_window) window_stack_remove(s_window, false);
}

void dest_picker_set_data(const char *data, uint16_t len)
{
    dest_parse(data, len);
    if (s_menu) menu_layer_reload_data(s_menu);
    /* If the user is inside a region window, the underlying rows may
     * have shifted — rebuild the letter index against the fresh data.
     * Region ordering from PKJS is stable (alphabetical), so the same
     * section index still refers to the same region in practice. */
    if (s_region_section >= 0 && s_region_menu) {
        rebuild_region_index();
        menu_layer_reload_data(s_region_menu);
    }
}

bool dest_picker_has_data(void)
{
    return g_dest_count > 0;
}

#endif /* MULTI_DESTINATION */
