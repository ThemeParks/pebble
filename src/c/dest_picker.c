/*
 * dest_picker.c — browse all ThemeParks.wiki destinations by proximity
 * and region. Uber-app only — per-destination builds skip this screen.
 *
 * Wire format from PKJS (pipe-delimited, newline-separated):
 *   H|Section Name          — section header (Nearby, Europe, etc.)
 *   D|id|name|parkCount|distKm   — destination row
 *
 * Sections appear in the order PKJS sends them: Nearby first (if GPS
 * available), then alphabetical regions.
 */

#include "config.h"

#ifdef MULTI_DESTINATION

#include "dest_picker.h"
#include "colours.h"
#include <pebble.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_SECTIONS 8
/* The API serves ~155 destinations (mid-2026) and grows over time —
 * keep headroom so the tail of the list isn't silently dropped. */
#define MAX_DESTS 200
#define MAX_DEST_NAME 32
#define MAX_DEST_ID 40

typedef struct {
    char id[MAX_DEST_ID];
    char name[MAX_DEST_NAME];
    int park_count;
    int dist_km;         /* 0 or -1 if unknown/not-shown */
    int section;
} dest_entry_t;

typedef struct {
    char name[MAX_DEST_NAME];
    int first_row;       /* index into s_dests where this section starts */
    int count;
} section_t;

static dest_entry_t s_dests[MAX_DESTS];
static section_t s_sections[MAX_SECTIONS];
static int s_dest_count = 0;
static int s_section_count = 0;

static Window *s_window = NULL;
static Layer *s_header = NULL;
static MenuLayer *s_menu = NULL;
static dest_picker_select_cb_t s_callback = NULL;

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

#define HEADER_HEIGHT PBL_IF_ROUND_ELSE(46, 32)
#define ROW_HEIGHT 38

/* --- Parse pipe-delimited payload ----------------------------------- */

static void parse_dest_data(const char *data, uint16_t len)
{
    s_dest_count = 0;
    s_section_count = 0;
    int pos = 0;
    int current_section = -1;

    while (pos < len && s_dest_count < MAX_DESTS) {
        /* Find end of line */
        int nl = pos;
        while (nl < len && data[nl] != '\n') nl++;
        int line_len = nl - pos;

        if (line_len < 3) {
            pos = nl + 1;
            continue;
        }

        if (data[pos] == 'H' && data[pos + 1] == '|') {
            /* Section header */
            if (s_section_count < MAX_SECTIONS) {
                section_t *sec = &s_sections[s_section_count];
                int name_len = line_len - 2;
                if (name_len > MAX_DEST_NAME - 1) name_len = MAX_DEST_NAME - 1;
                memcpy(sec->name, data + pos + 2, name_len);
                sec->name[name_len] = '\0';
                sec->first_row = s_dest_count;
                sec->count = 0;
                current_section = s_section_count;
                s_section_count++;
            }
        } else if (data[pos] == 'D' && data[pos + 1] == '|') {
            /* Destination row: D|id|name|parkCount|distKm */
            const char *p = data + pos + 2;
            const char *end = data + nl;
            const char *fs = p;
            int fi = 0;
            const char *fields[4] = {NULL, NULL, NULL, NULL};
            int flens[4] = {0, 0, 0, 0};
            for (const char *c = p; c <= end && fi < 4; c++) {
                if (c == end || *c == '|') {
                    fields[fi] = fs;
                    flens[fi] = (int)(c - fs);
                    fi++;
                    fs = c + 1;
                }
            }
            if (fi >= 3) {
                dest_entry_t *d = &s_dests[s_dest_count];
                int id_len = flens[0] < MAX_DEST_ID - 1 ? flens[0] : MAX_DEST_ID - 1;
                memcpy(d->id, fields[0], id_len);
                d->id[id_len] = '\0';
                int nm_len = flens[1] < MAX_DEST_NAME - 1 ? flens[1] : MAX_DEST_NAME - 1;
                memcpy(d->name, fields[1], nm_len);
                d->name[nm_len] = '\0';
                char tmp[8];
                int c2_len = flens[2] < 7 ? flens[2] : 7;
                memcpy(tmp, fields[2], c2_len);
                tmp[c2_len] = '\0';
                d->park_count = atoi(tmp);
                if (fi >= 4) {
                    int c3_len = flens[3] < 7 ? flens[3] : 7;
                    memcpy(tmp, fields[3], c3_len);
                    tmp[c3_len] = '\0';
                    d->dist_km = atoi(tmp);
                } else {
                    d->dist_km = 0;
                }
                d->section = current_section;
                s_dest_count++;
                if (current_section >= 0 && current_section < s_section_count) {
                    s_sections[current_section].count++;
                }
            }
        }
        pos = nl + 1;
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "dest_picker: %d dests in %d sections",
            s_dest_count, s_section_count);
}

/* --- header --------------------------------------------------------- */

static void header_update_proc(Layer *layer, GContext *gctx)
{
    GRect b = layer_get_bounds(layer);
    graphics_context_set_fill_color(gctx, COL_HEADER_BG);
    graphics_fill_rect(gctx, b, 0, GCornerNone);
    graphics_context_set_text_color(gctx, COL_HEADER_FG);
    const int TEXT_Y = PBL_IF_ROUND_ELSE(16, 2);
    graphics_draw_text(gctx, "Pick a Park",
        fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
        GRect(b.origin.x + 4, b.origin.y + TEXT_Y,
              b.size.w - 8, b.size.h - TEXT_Y),
        GTextOverflowModeTrailingEllipsis,
        GTextAlignmentCenter, NULL);
}

/* --- menu callbacks ------------------------------------------------- */

static uint16_t menu_get_num_sections(struct MenuLayer *l, void *ctx)
{
    return s_section_count > 0 ? s_section_count : 1;
}

static uint16_t menu_get_num_rows(struct MenuLayer *l, uint16_t section, void *ctx)
{
    if (s_section_count == 0) return 1;  /* "Loading..." placeholder */
    if (section < s_section_count) return s_sections[section].count;
    return 0;
}

static int16_t menu_get_header_height(struct MenuLayer *l, uint16_t section, void *ctx)
{
    if (s_section_count == 0) return 0;
    return 18;
}

static void menu_draw_header(GContext *gctx, const Layer *cell,
                             uint16_t section, void *ctx)
{
    if (s_section_count == 0) return;
    GRect b = layer_get_bounds(cell);
    graphics_context_set_fill_color(gctx, GColorLightGray);
    graphics_fill_rect(gctx, b, 0, GCornerNone);
    graphics_context_set_text_color(gctx, GColorBlack);
    const char *title = (section < s_section_count) ? s_sections[section].name : "";
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

static int16_t menu_get_cell_height(struct MenuLayer *l, MenuIndex *idx, void *ctx)
{
    return ROW_HEIGHT;
}

static dest_entry_t *dest_at(MenuIndex *idx)
{
    if (idx->section >= s_section_count) return NULL;
    int i = s_sections[idx->section].first_row + idx->row;
    if (i < 0 || i >= s_dest_count) return NULL;
    return &s_dests[i];
}

static void menu_draw_row(GContext *gctx, const Layer *cell,
                          MenuIndex *idx, void *ctx)
{
    GRect b = layer_get_bounds(cell);
    bool selected = menu_cell_layer_is_highlighted(cell);

    if (s_dest_count == 0) {
        /* Placeholder row when no data yet */
        graphics_context_set_text_color(gctx, GColorBlack);
        graphics_draw_text(gctx, "Loading destinations...",
            fonts_get_system_font(FONT_KEY_GOTHIC_18),
            GRect(b.origin.x + 8, b.origin.y + 8, b.size.w - 16, 24),
            GTextOverflowModeTrailingEllipsis,
            GTextAlignmentCenter, NULL);
        return;
    }

    dest_entry_t *d = dest_at(idx);
    if (!d) return;

    /* Background (zebra stripe when not selected — MenuLayer paints
     * the highlight for selected rows via the colors set in window_load).
     * On round, skip the stripe: a full-width fill clips hard against
     * the circle mask and visually overrides any curve-following the
     * text does. Plain white lets the curved display read naturally. */
#ifndef PBL_ROUND
    if (!selected) {
        GColor bg = (idx->row & 1) ? COL_ROW_STRIPE : GColorWhite;
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

    /* Subtitle: distance if we have it, otherwise park count */
    char subtitle[24];
    if (d->dist_km > 0) {
        if (d->dist_km < 2) {
            snprintf(subtitle, sizeof(subtitle), "< 1 km");
        } else {
            snprintf(subtitle, sizeof(subtitle), "%d km", d->dist_km);
        }
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

static void menu_select(struct MenuLayer *l, MenuIndex *idx, void *ctx)
{
    dest_entry_t *d = dest_at(idx);
    if (!d || !s_callback) return;
    APP_LOG(APP_LOG_LEVEL_INFO, "dest_picker: selected %s", d->name);
    s_callback(d->id, d->park_count);
}

/* --- window lifecycle ----------------------------------------------- */

static void window_load(Window *w)
{
    Layer *root = window_get_root_layer(w);
    GRect bounds = layer_get_bounds(root);

    s_header = layer_create(GRect(0, 0, bounds.size.w, HEADER_HEIGHT));
    layer_set_update_proc(s_header, header_update_proc);
    layer_add_child(root, s_header);

    s_menu = menu_layer_create(GRect(0, HEADER_HEIGHT, bounds.size.w,
                                     bounds.size.h - HEADER_HEIGHT));
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
}

static void window_unload(Window *w)
{
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
    if (s_window) window_stack_remove(s_window, false);
}

void dest_picker_set_data(const char *data, uint16_t len)
{
    parse_dest_data(data, len);
    if (s_menu) menu_layer_reload_data(s_menu);
}

#endif /* MULTI_DESTINATION */
