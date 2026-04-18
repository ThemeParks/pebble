/*
 * park_picker.h — initial park selection menu.
 */
#pragma once

typedef void (*park_picker_select_cb_t)(int park_index);

void park_picker_show(park_picker_select_cb_t on_select);
void park_picker_hide(void);
