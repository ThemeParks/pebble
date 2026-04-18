/*
 * park_view.h — main park view (attractions + shows tabs).
 *
 * Reads from g_park (parkdata.c). Re-rendering after a park switch is
 * just a call to park_view_data_changed() — no allocation.
 */
#pragma once

typedef void (*park_view_back_cb_t)(void);
typedef void (*park_view_settings_cb_t)(void);

void park_view_show(const char *park_id,
                    park_view_back_cb_t on_back,
                    park_view_settings_cb_t on_settings);
void park_view_hide(void);
void park_view_data_changed(void);   /* re-sort + re-render */
