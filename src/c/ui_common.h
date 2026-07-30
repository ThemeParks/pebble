/*
 * ui_common.h — shared interaction conventions for list screens.
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

#define SCROLL_REPEAT_MS 100

/* Frame animations (header slides etc.) — short enough to never feel
 * like waiting, long enough to read as motion. */
#define UI_ANIM_MS 150
