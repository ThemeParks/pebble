/*
 * ui_common.h — shared interaction conventions for list screens.
 *
 * Scrolling rule (device-tested, see park_view.c history): UP/DOWN move
 * the cursor ONE row instantly (no scroll animation — easing reads as
 * input lag on a one-row hop) and repeat at SCROLL_REPEAT_MS while
 * held. NEVER add a multi-click subscription to a scroll button: the
 * firmware then holds every single click for the ~300ms multi-click
 * disambiguation window, which feels like lag on each press. Held
 * repeat covers even the longest lists (~50 rows) in ~5s, so no coarse
 * jump gesture is needed.
 */
#pragma once

#define SCROLL_REPEAT_MS 100
