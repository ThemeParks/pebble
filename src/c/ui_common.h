/*
 * ui_common.h — shared interaction + window-chrome conventions.
 *
 * Scrolling rule (device-tested, see park_view.c history): UP/DOWN move
 * the cursor ONE row per press, ANIMATED — the eased scroll is what
 * makes the native menus feel polished, and it is NOT the source of
 * perceived input lag as long as the motion starts on button-down.
 * The actual lag source is multi-click: NEVER add a multi-click
 * subscription to a scroll button — the firmware then holds every
 * single click for the ~300ms disambiguation window, and every press
 * feels delayed. Buttons repeat at SCROLL_REPEAT_MS while held, which
 * covers even the longest lists (~50 rows) in ~5s, so no coarse jump
 * gesture is needed.
 */
#pragma once

#include <pebble.h>

#define SCROLL_REPEAT_MS 100

/* Frame animations (header slides etc.) — short enough to never feel
 * like waiting, long enough to read as motion. */
#define UI_ANIM_MS 150

/* Every window opens with a status bar showing the current time. A park
 * day runs on the clock (showtimes, closing time, when to head for the
 * next ride), and tester feedback was that checking it meant leaving
 * the app for the watchface. The firmware draws and ticks the bar and
 * follows the watch's 12/24h setting. On round it also fills the top
 * band the circle clips anyway, which headers used to pad around.
 * Titled windows (pickers, settings) put a UI_TITLE_H strip under it. */
#define UI_STATUS_H STATUS_BAR_LAYER_HEIGHT
#define UI_TITLE_H 26
#define UI_TITLE_TEXT_Y 2

/* Create the time status bar in the header palette (it reads as the top
 * of the window's header block) and add it to `root`. Call it after the
 * window's other layers so it draws on top — the park view's header
 * slides up underneath it. NULL on OOM: the window just has no clock. */
StatusBarLayer *ui_status_bar_create(Layer *root);
