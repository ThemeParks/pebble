/*
 * settings_view.h — sort + favourites menu.
 */
#pragma once

typedef void (*settings_back_cb_t)(void);
typedef void (*settings_refresh_cb_t)(void);

/* on_refresh fires when the user picks "Refresh Data" — main.c closes
 * settings and reloads the current park bypassing the phone cache. */
void settings_view_show(const char *park_id, settings_back_cb_t on_back,
                        settings_refresh_cb_t on_refresh);
void settings_view_hide(void);
