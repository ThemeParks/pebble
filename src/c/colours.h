/*
 * colours.h — app colour palette. On colour platforms (emery, basalt,
 * chalk) we use themed hues; on b&w (gabbro/diorite) the macros fall
 * back to black/white via the PBL_COLOR/PBL_BW capability.
 *
 * Traffic-light mapping for wait times:
 *   < 30 min — green
 *   30..59   — amber
 *   >= 60    — red
 *   null / closed / down / rehab — dim grey (meta info, not a wait)
 */
#pragma once

#include <pebble.h>

/* Header + tabs: use branded colours so the app feels like "the theme-park app". */
#define COL_HEADER_BG    PBL_IF_COLOR_ELSE(GColorOxfordBlue, GColorBlack)
#define COL_HEADER_FG    GColorWhite
#define COL_HEADER_HOURS PBL_IF_COLOR_ELSE(GColorCeleste, GColorWhite)

#define COL_TAB_ACTIVE_BG   PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorBlack)
#define COL_TAB_ACTIVE_FG   GColorWhite
#define COL_TAB_INACTIVE_BG PBL_IF_COLOR_ELSE(GColorLightGray, GColorLightGray)
#define COL_TAB_INACTIVE_FG PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack)

/* Row styling */
#define COL_ROW_FG          GColorBlack
/* Subtle zebra stripe — even rows keep the white body, odd rows get a
 * pale accent so the eye can track which wait time belongs to which ride.
 * Matters most on gabbro where the circle can visually decouple them. */
#define COL_ROW_STRIPE      PBL_IF_COLOR_ELSE(GColorCeleste, GColorWhite)
#define COL_ROW_SELECTED_BG PBL_IF_COLOR_ELSE(GColorPictonBlue, GColorBlack)
#define COL_ROW_SELECTED_FG GColorWhite

/* Wait-time traffic light */
#define COL_WAIT_GREEN PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorBlack)
#define COL_WAIT_AMBER PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorBlack)
#define COL_WAIT_RED   PBL_IF_COLOR_ELSE(GColorRed, GColorBlack)
#define COL_WAIT_DIM   PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack)
