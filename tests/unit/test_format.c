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

/* --- glance line ------------------------------------------------------ */

static void test_glance_hours_rung(void)
{
	char out[64];
	glance_input_t in = {
		.park_name = "Magic Kingdom",
		.has_schedule = true,
		.open_hhmm = "09:00",
		.close_hhmm = "22:00",
		.hours_current = true,
		.wait_mins = -1,
	};
	ASSERT_EQ_INT(GLANCE_RUNG_HOURS,
	              format_glance_line(&in, false, out, sizeof(out)));
	ASSERT_EQ_STR("Magic Kingdom · 9 AM - 10 PM", out);
	ASSERT_EQ_INT(GLANCE_RUNG_HOURS,
	              format_glance_line(&in, true, out, sizeof(out)));
	ASSERT_EQ_STR("Magic Kingdom · 9:00 - 22:00", out);
}

static void test_glance_wait_rung(void)
{
	char out[64];
	glance_input_t in = {
		.park_name = "MK",
		.ride_name = "Space Mtn",
		.wait_mins = 45,
		.wait_age_secs = 5 * 60,
	};
	ASSERT_EQ_INT(GLANCE_RUNG_WAIT,
	              format_glance_line(&in, false, out, sizeof(out)));
	ASSERT_EQ_STR("Space Mtn 45m · MK", out);

	/* A 0-minute wait is a real (great!) wait, not "no data". */
	in.wait_mins = 0;
	ASSERT_EQ_INT(GLANCE_RUNG_WAIT,
	              format_glance_line(&in, false, out, sizeof(out)));
	ASSERT_EQ_STR("Space Mtn 0m · MK", out);

	/* At/over the freshness cutoff the wait must not be shown. */
	in.wait_mins = 45;
	in.wait_age_secs = GLANCE_WAIT_MAX_AGE_SECS;
	ASSERT_EQ_INT(GLANCE_RUNG_NONE,
	              format_glance_line(&in, false, out, sizeof(out)));
	ASSERT_EQ_STR("", out);
}

static void test_glance_precedence(void)
{
	/* Both rungs available → hours win. */
	char out[64];
	glance_input_t in = {
		.park_name = "Epcot",
		.has_schedule = true,
		.open_hhmm = "09:00",
		.close_hhmm = "21:00",
		.hours_current = true,
		.ride_name = "Test Track",
		.wait_mins = 30,
		.wait_age_secs = 60,
	};
	ASSERT_EQ_INT(GLANCE_RUNG_HOURS,
	              format_glance_line(&in, false, out, sizeof(out)));
	ASSERT_EQ_STR("Epcot · 9 AM - 9 PM", out);

	/* Hours known but past close → fresh wait takes over. */
	in.hours_current = false;
	ASSERT_EQ_INT(GLANCE_RUNG_WAIT,
	              format_glance_line(&in, false, out, sizeof(out)));
	ASSERT_EQ_STR("Test Track 30m · Epcot", out);
}

static void test_glance_none(void)
{
	char out[64];
	glance_input_t in = { .park_name = "MK", .wait_mins = -1 };
	ASSERT_EQ_INT(GLANCE_RUNG_NONE,
	              format_glance_line(&in, false, out, sizeof(out)));
	ASSERT_EQ_STR("", out);
	/* Defensive: NULL input and zero-size output must not blow up. */
	ASSERT_EQ_INT(GLANCE_RUNG_NONE,
	              format_glance_line(NULL, false, out, sizeof(out)));
	ASSERT_EQ_STR("", out);
	ASSERT_EQ_INT(GLANCE_RUNG_NONE, format_glance_line(&in, false, out, 0));
}

static void test_glance_no_park_name(void)
{
	/* No park name (e.g. it was never set): drop the prefix/suffix
	 * rather than rendering a dangling " · ". */
	char out[64];
	glance_input_t in = {
		.park_name = "",
		.has_schedule = true,
		.open_hhmm = "09:00",
		.close_hhmm = "21:00",
		.hours_current = true,
		.wait_mins = -1,
	};
	ASSERT_EQ_INT(GLANCE_RUNG_HOURS,
	              format_glance_line(&in, false, out, sizeof(out)));
	ASSERT_EQ_STR("9 AM - 9 PM", out);

	glance_input_t in2 = {
		.park_name = NULL,
		.ride_name = "Space Mtn",
		.wait_mins = 5,
	};
	ASSERT_EQ_INT(GLANCE_RUNG_WAIT,
	              format_glance_line(&in2, false, out, sizeof(out)));
	ASSERT_EQ_STR("Space Mtn 5m", out);
}

static void test_glance_truncation_safe(void)
{
	/* Max-length names into a deliberately tiny buffer: must truncate
	 * to outsz-1 + NUL, never overrun. */
	char out[24];
	glance_input_t in = {
		.park_name = "Disney's Animal Kingdom",             /* 23 chars */
		.ride_name =
			"Expedition Everest - Legend of the Mtn",  /* 39 chars */
		.wait_mins = 120,
		.wait_age_secs = 0,
	};
	ASSERT_EQ_INT(GLANCE_RUNG_WAIT,
	              format_glance_line(&in, false, out, sizeof(out)));
	ASSERT_EQ_INT((int)sizeof(out) - 1, (int)strlen(out));
	ASSERT_EQ_STR("Expedition Everest - Le", out);

	in.has_schedule = true;
	in.open_hhmm = "08:00";
	in.close_hhmm = "23:30";
	in.hours_current = true;
	ASSERT_EQ_INT(GLANCE_RUNG_HOURS,
	              format_glance_line(&in, true, out, sizeof(out)));
	ASSERT_EQ_INT((int)sizeof(out) - 1, (int)strlen(out));
	/* "Disney's Animal Kingdom" is exactly 23 chars, so the whole tail
	 * (" · 8:00 - 23:30") is what gets cut. */
	ASSERT_EQ_STR("Disney's Animal Kingdom", out);
}

static void test_glance_countdown_template(void)
{
	/* Pins the exact AppGlance template text: the launcher parses the
	 * {time_until(...)|format(...)} braces at render time, so any drift
	 * here would show up as literal braces on the launcher. */
	char out[96];
	format_glance_countdown("Magic Kingdom", "closes", 1753797600u,
	                        out, sizeof(out));
	ASSERT_EQ_STR(
		"Magic Kingdom · closes in {time_until(1753797600)|format('%aR')}",
		out);
	format_glance_countdown("Epcot", "opens", 1753772400u,
	                        out, sizeof(out));
	ASSERT_EQ_STR(
		"Epcot · opens in {time_until(1753772400)|format('%aR')}", out);
}

static void test_glance_countdown_no_park(void)
{
	/* Empty/NULL park drops the prefix, not just the name. */
	char out[96];
	format_glance_countdown("", "closes", 1u, out, sizeof(out));
	ASSERT_EQ_STR("closes in {time_until(1)|format('%aR')}", out);
	format_glance_countdown(NULL, "opens", 1u, out, sizeof(out));
	ASSERT_EQ_STR("opens in {time_until(1)|format('%aR')}", out);
}

static void test_glance_countdown_safe(void)
{
	/* Truncation must clamp at outsz-1 + NUL, never overrun. (Callers
	 * are expected to size buffers so this never happens for real —
	 * a cut template renders literally.) */
	char tiny[16];
	format_glance_countdown("Magic Kingdom", "closes", 1753797600u,
	                        tiny, sizeof(tiny));
	ASSERT_EQ_INT((int)sizeof(tiny) - 1, (int)strlen(tiny));

	/* Degenerate inputs must not blow up or emit half a template. */
	char out[96];
	format_glance_countdown("MK", NULL, 1u, out, sizeof(out));
	ASSERT_EQ_STR("", out);
	format_glance_countdown("MK", "", 1u, out, sizeof(out));
	ASSERT_EQ_STR("", out);
	format_glance_countdown("MK", "closes", 1u, out, 0);  /* no crash */
}

static void test_glance_countdown_max_epoch(void)
{
	/* The widest epoch (2^32-1) must still fit main.c's 96-byte slots
	 * with the longest real park name. */
	char out[96];
	format_glance_countdown("Disney's Hollywood Studios", "closes",
	                        4294967295u, out, sizeof(out));
	ASSERT_EQ_STR(
		"Disney's Hollywood Studios · closes in "
		"{time_until(4294967295)|format('%aR')}", out);
	ASSERT_TRUE((int)strlen(out) < 96 - 1);
}

static void test_glance_hours_current(void)
{
	/* Normal daytime schedule: open 09:00, close 22:00. */
	ASSERT_TRUE(format_hours_current(12 * 60, 9 * 60, 22 * 60));  /* midday */
	ASSERT_TRUE(format_hours_current(7 * 60, 9 * 60, 22 * 60));   /* pre-open: informative */
	ASSERT_TRUE(!format_hours_current(22 * 60, 9 * 60, 22 * 60)); /* at close */
	ASSERT_TRUE(!format_hours_current(23 * 60, 9 * 60, 22 * 60)); /* after close */

	/* Past-midnight close: open 09:00, close 01:00 (next day). */
	ASSERT_TRUE(format_hours_current(23 * 60, 9 * 60, 1 * 60));       /* 23:00 */
	ASSERT_TRUE(format_hours_current(0 * 60 + 30, 9 * 60, 1 * 60));   /* 00:30 */
	ASSERT_TRUE(!format_hours_current(2 * 60, 9 * 60, 1 * 60));       /* 02:00 */

	/* Out-of-range minutes are never "current". */
	ASSERT_TRUE(!format_hours_current(-1, 9 * 60, 22 * 60));
	ASSERT_TRUE(!format_hours_current(12 * 60, -1, 22 * 60));
	ASSERT_TRUE(!format_hours_current(12 * 60, 9 * 60, 24 * 60));
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
	RUN_TEST(test_glance_hours_rung);
	RUN_TEST(test_glance_wait_rung);
	RUN_TEST(test_glance_precedence);
	RUN_TEST(test_glance_none);
	RUN_TEST(test_glance_no_park_name);
	RUN_TEST(test_glance_truncation_safe);
	RUN_TEST(test_glance_countdown_template);
	RUN_TEST(test_glance_countdown_no_park);
	RUN_TEST(test_glance_countdown_safe);
	RUN_TEST(test_glance_countdown_max_epoch);
	RUN_TEST(test_glance_hours_current);
	return unit_summary();
}
