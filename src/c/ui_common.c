/*
 * ui_common.c — shared window chrome. Conventions live in ui_common.h.
 */
#include "ui_common.h"
#include "colours.h"

StatusBarLayer *ui_status_bar_create(Layer *root)
{
	StatusBarLayer *bar = status_bar_layer_create();
	if (!bar) return NULL;
	/* No separator: the bar and the header below share one colour and
	 * read as a single block. */
	status_bar_layer_set_colors(bar, COL_HEADER_BG, COL_HEADER_FG);
	status_bar_layer_set_separator_mode(bar, StatusBarLayerSeparatorModeNone);
	layer_add_child(root, status_bar_layer_get_layer(bar));
	return bar;
}
