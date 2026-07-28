/*
 * dest_picker.h — destination selection menu (uber app only).
 *
 * Receives a pipe-delimited list of destinations from PKJS via
 * dest_picker_set_data(), renders a sectioned menu (Nearby + regions),
 * fires the select callback when the user picks one.
 */
#pragma once

#ifdef MULTI_DESTINATION

#include <stdint.h>
#include <stdbool.h>

typedef void (*dest_picker_select_cb_t)(const char *dest_id, int park_count);

/* Show the destination picker window. Triggers on_select when user picks. */
void dest_picker_show(dest_picker_select_cb_t on_select);

/* Hide/remove the window from the stack. */
void dest_picker_hide(void);

/* Called by api.c when DEST_LIST_DATA arrives from PKJS. Parses the
 * pipe-delimited payload into internal storage and refreshes the menu. */
void dest_picker_set_data(const char *data, uint16_t len);

/* True once a destination list has been parsed. Lets main.c decide
 * whether to re-request the list when revealing the picker. */
bool dest_picker_has_data(void);

#endif
