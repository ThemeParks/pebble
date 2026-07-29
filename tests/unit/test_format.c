/*
 * test_format.c — host-side tests for the pure display formatters.
 *
 * The wire format for times is zero-padded 24h "HH:MM" (produced by
 * PKJS formatTime); format_clock_time turns that into the user-facing
 * string per the watch's 12/24h preference. Legacy persisted strings
 * ("9 AM" from older app versions) must pass through untouched.
 */
#include "format.h"
#include "unit.h"
#include <string.h>

static void test_clock_24h(void)
{
	char out[16];
	ASSERT_TRUE(format_clock_time("09:00", true, out, sizeof(out)));
	ASSERT_EQ_STR("9:00", out);
	ASSERT_TRUE(format_clock_time("21:30", true, out, sizeof(out)));
	ASSERT_EQ_STR("21:30", out);
	ASSERT_TRUE(format_clock_time("00:05", true, out, sizeof(out)));
	ASSERT_EQ_STR("0:05", out);
}

static void test_clock_12h(void)
{
	char out[16];
	ASSERT_TRUE(format_clock_time("09:00", false, out, sizeof(out)));
	ASSERT_EQ_STR("9 AM", out);
	ASSERT_TRUE(format_clock_time("12:00", false, out, sizeof(out)));
	ASSERT_EQ_STR("12 PM", out);
	ASSERT_TRUE(format_clock_time("00:30", false, out, sizeof(out)));
	ASSERT_EQ_STR("12:30 AM", out);
	ASSERT_TRUE(format_clock_time("13:05", false, out, sizeof(out)));
	ASSERT_EQ_STR("1:05 PM", out);
	ASSERT_TRUE(format_clock_time("23:59", false, out, sizeof(out)));
	ASSERT_EQ_STR("11:59 PM", out);
}

static void test_clock_legacy_passthrough(void)
{
	/* Older app versions persisted pre-formatted hours ("9 AM"); after
	 * an update those cached strings must still render, not blank out. */
	char out[16];
	ASSERT_TRUE(!format_clock_time("9 AM", false, out, sizeof(out)));
	ASSERT_EQ_STR("9 AM", out);
	ASSERT_TRUE(!format_clock_time("10:30 PM", true, out, sizeof(out)));
	ASSERT_EQ_STR("10:30 PM", out);
	ASSERT_TRUE(!format_clock_time("", true, out, sizeof(out)));
	ASSERT_EQ_STR("", out);
	ASSERT_TRUE(!format_clock_time(NULL, true, out, sizeof(out)));
	ASSERT_EQ_STR("", out);
}

static void test_clock_range_validation(void)
{
	/* Digits in the right shape but out of range are not times. */
	char out[16];
	ASSERT_TRUE(!format_clock_time("25:00", true, out, sizeof(out)));
	ASSERT_EQ_STR("25:00", out);
	ASSERT_TRUE(!format_clock_time("12:60", false, out, sizeof(out)));
	ASSERT_EQ_STR("12:60", out);
	/* Trailing junk breaks the strict parse. */
	ASSERT_TRUE(!format_clock_time("09:00x", true, out, sizeof(out)));
	ASSERT_EQ_STR("09:00x", out);
}

static void test_clock_truncation_safe(void)
{
	/* A too-small buffer must truncate, not overrun. */
	char out[4];
	format_clock_time("21:30", true, out, sizeof(out));
	ASSERT_EQ_STR("21:", out);
	format_clock_time("legacy string that is long", true, out, sizeof(out));
	ASSERT_EQ_STR("leg", out);
}

static void test_distance_km(void)
{
	char out[16];
	format_distance(0, false, out, sizeof(out));
	ASSERT_EQ_STR("< 1 km", out);
	format_distance(1, false, out, sizeof(out));
	ASSERT_EQ_STR("< 1 km", out);
	format_distance(2, false, out, sizeof(out));
	ASSERT_EQ_STR("2 km", out);
	format_distance(12, false, out, sizeof(out));
	ASSERT_EQ_STR("12 km", out);
}

static void test_distance_miles(void)
{
	char out[16];
	format_distance(1, true, out, sizeof(out));    /* 0.62 mi rounds to 1 */
	ASSERT_EQ_STR("1 mi", out);
	format_distance(0, true, out, sizeof(out));
	ASSERT_EQ_STR("< 1 mi", out);
	format_distance(2, true, out, sizeof(out));    /* 1.24 → 1 */
	ASSERT_EQ_STR("1 mi", out);
	format_distance(8, true, out, sizeof(out));    /* 4.97 → 5 */
	ASSERT_EQ_STR("5 mi", out);
	format_distance(100, true, out, sizeof(out));  /* 62.1 → 62 */
	ASSERT_EQ_STR("62 mi", out);
}

static void test_locale_miles(void)
{
	ASSERT_TRUE(format_locale_uses_miles("en_US"));
	ASSERT_TRUE(format_locale_uses_miles("en_GB"));
	ASSERT_TRUE(!format_locale_uses_miles("de_DE"));
	ASSERT_TRUE(!format_locale_uses_miles("fr_FR"));
	ASSERT_TRUE(!format_locale_uses_miles("en"));
	ASSERT_TRUE(!format_locale_uses_miles(NULL));
}

static void test_age_suffix(void)
{
	char out[16];
	const uint32_t T0 = 1753776000u;

	/* Unknown timestamp → no suffix. */
	format_age_suffix(T0, 0, out, sizeof(out));
	ASSERT_EQ_STR("", out);
	/* Fresh (<2 min) → no suffix. */
	format_age_suffix(T0 + 119, T0, out, sizeof(out));
	ASSERT_EQ_STR("", out);
	/* Minutes. */
	format_age_suffix(T0 + 3 * 60, T0, out, sizeof(out));
	ASSERT_EQ_STR(" · 3m", out);
	format_age_suffix(T0 + 59 * 60, T0, out, sizeof(out));
	ASSERT_EQ_STR(" · 59m", out);
	/* Hours. */
	format_age_suffix(T0 + 60 * 60, T0, out, sizeof(out));
	ASSERT_EQ_STR(" · 1h", out);
	format_age_suffix(T0 + 5 * 3600 + 90, T0, out, sizeof(out));
	ASSERT_EQ_STR(" · 5h", out);
	/* Clock skew: updated in the future → no suffix. */
	format_age_suffix(T0, T0 + 60, out, sizeof(out));
	ASSERT_EQ_STR("", out);
}

int main(void)
{
	RUN_TEST(test_clock_24h);
	RUN_TEST(test_clock_12h);
	RUN_TEST(test_clock_legacy_passthrough);
	RUN_TEST(test_clock_range_validation);
	RUN_TEST(test_clock_truncation_safe);
	RUN_TEST(test_distance_km);
	RUN_TEST(test_distance_miles);
	RUN_TEST(test_locale_miles);
	RUN_TEST(test_age_suffix);
	return unit_summary();
}
