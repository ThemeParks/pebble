/*
 * test_geom.c — host-side tests for the round-display geometry.
 *
 * geom_round_span places the park view's tab labels and gear inside the
 * visible circle on gabbro (260x260), wherever the strip slides to.
 */
#include "geom.h"
#include "unit.h"

static void test_span_centre_row_is_full_width(void)
{
	int x0, x1;
	geom_round_span(260, 130, &x0, &x1);
	ASSERT_EQ_INT(0, x0);
	ASSERT_EQ_INT(260, x1);
}

static void test_span_is_symmetric_about_centre(void)
{
	/* Rows equidistant from the middle see the same chord. */
	int a0, a1, b0, b1;
	geom_round_span(260, 40, &a0, &a1);
	geom_round_span(260, 220, &b0, &b1);
	ASSERT_EQ_INT(a0, b0);
	ASSERT_EQ_INT(a1, b1);
	ASSERT_EQ_INT(260, a0 + a1);   /* centred on x = 130 */
}

static void test_span_values_under_status_bar(void)
{
	/* The rows the park view actually asks about on gabbro: label tops of
	 * the tab strip collapsed (y=25) and expanded (y=61). */
	int x0, x1;
	geom_round_span(260, 25, &x0, &x1);    /* sqrt(130^2 - 105^2) = 76.6 */
	ASSERT_EQ_INT(54, x0);
	ASSERT_EQ_INT(206, x1);
	geom_round_span(260, 61, &x0, &x1);    /* sqrt(130^2 - 69^2) = 110.2 */
	ASSERT_EQ_INT(20, x0);
	ASSERT_EQ_INT(240, x1);
}

static void test_span_edges_and_outside(void)
{
	int x0, x1;
	/* Top row: the circle only grazes it — empty span at the centre. */
	geom_round_span(260, 0, &x0, &x1);
	ASSERT_EQ_INT(130, x0);
	ASSERT_EQ_INT(130, x1);
	/* Bottom row still has a sliver. */
	geom_round_span(260, 259, &x0, &x1);   /* sqrt(130^2 - 129^2) = 16.1 */
	ASSERT_EQ_INT(114, x0);
	ASSERT_EQ_INT(146, x1);
	/* Off-screen rows (a strip mid-slide can start above y=0). */
	geom_round_span(260, -5, &x0, &x1);
	ASSERT_EQ_INT(x0, x1);
	geom_round_span(260, 300, &x0, &x1);
	ASSERT_EQ_INT(x0, x1);
}

static void test_span_widens_towards_centre(void)
{
	/* Monotonic: every row nearer the middle is at least as wide. */
	int prev = -1;
	for (int y = 0; y <= 130; y++) {
		int x0, x1;
		geom_round_span(260, y, &x0, &x1);
		ASSERT_TRUE(x1 - x0 >= prev);
		prev = x1 - x0;
	}
}

int main(void)
{
	RUN_TEST(test_span_centre_row_is_full_width);
	RUN_TEST(test_span_is_symmetric_about_centre);
	RUN_TEST(test_span_values_under_status_bar);
	RUN_TEST(test_span_edges_and_outside);
	RUN_TEST(test_span_widens_towards_centre);
	return unit_summary();
}
