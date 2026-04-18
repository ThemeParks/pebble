/*
 * settings_view.h — sort + favourites menu.
 */
#pragma once

typedef void (*settings_back_cb_t)(void);

void settings_view_show(const char *park_id, settings_back_cb_t on_back);
void settings_view_hide(void);
