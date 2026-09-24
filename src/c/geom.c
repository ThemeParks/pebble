/*
 * geom.c — pure screen geometry. See geom.h for the contracts.
 *
 * Kept free of <pebble.h> on purpose: scripts/unit_tests.py compiles
 * this file on the host and test_geom.c exercises it.
 */
#include "geom.h"

/* floor(sqrt(n)) by Newton's method — integer only, no libm needed. */
static int isqrt(int n)
{
	if (n <= 0) return 0;
	int x = n;
	int y = (x + 1) / 2;
	while (y < x) {
		x = y;
		y = (x + n / x) / 2;
	}
	return x;
}

void geom_round_span(int diameter, int y, int *x0, int *x1)
{
	int r = diameter / 2;
	int hw = 0;
	if (y >= 0 && y < diameter) {
		int dy = y - r;
		hw = isqrt(r * r - dy * dy);
	}
	*x0 = r - hw;
	*x1 = r + hw;
}
