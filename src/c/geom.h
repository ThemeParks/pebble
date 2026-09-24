/*
 * geom.h — pure screen geometry (no Pebble SDK dependency).
 *
 * Host-compilable so scripts/unit_tests.py can exercise it off-watch.
 */
#pragma once

/* Visible horizontal span of screen row `y` on a round display: the
 * chord of the circle inscribed in a diameter x diameter screen, as the
 * half-open range [*x0, *x1). Rows outside the circle get an empty span
 * at the centre (x0 == x1). Rectangular screens don't need this; their
 * span is the full width. */
void geom_round_span(int diameter, int y, int *x0, int *x1);
