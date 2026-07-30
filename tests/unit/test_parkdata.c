/*
 * test_parkdata.c — host-side tests for the pipe-format parser.
 *
 * Payloads are built to match the wire format documented at the top of
 * parkdata.c: schedule line, timestamp line, then A|/S| entity lines.
 */
#include "parkdata.h"
#include "unit.h"
#include <string.h>
#include <stdio.h>

static bool parse_str(const char *raw)
{
	return parkdata_parse(raw, (uint16_t)strlen(raw));
}

static void test_schedule_present(void)
{
	ASSERT_TRUE(parse_str("9:00 AM|10:00 PM\n1753776000\n"));
	ASSERT_TRUE(g_park.has_schedule);
	ASSERT_EQ_STR("9:00 AM", g_park.schedule_open);
	ASSERT_EQ_STR("10:00 PM", g_park.schedule_close);
}

static void test_schedule_absent(void)
{
	/* Empty line 0 means "no schedule known" — must not shift parsing
	 * of the following lines. */
	ASSERT_TRUE(parse_str("\n1753776000\nA|aaaa|Ride|O|10\n"));
	ASSERT_TRUE(!g_park.has_schedule);
	ASSERT_EQ_STR("", g_park.schedule_open);
	ASSERT_EQ_STR("", g_park.schedule_close);
	ASSERT_EQ_INT(1, g_park.attraction_count);
	ASSERT_EQ_STR("Ride", g_park.attractions[0].name);
}

static void test_schedule_no_pipe(void)
{
	/* A non-empty line 0 without a '|' separator is not a schedule. */
	ASSERT_TRUE(parse_str("TBD\n1753776000\n"));
	ASSERT_TRUE(!g_park.has_schedule);
}

static void test_timestamp_seconds_roundtrip(void)
{
	/* Seconds-scale value must round-trip exactly. (PKJS currently sends
	 * milliseconds — a known bug fixed by a later task — so we only pin
	 * the seconds contract here.) */
	ASSERT_TRUE(parse_str("\n1753776000\n"));
	ASSERT_EQ_INT(1753776000, (long long)g_park.last_updated_secs);
}

static void test_timestamp_garbage_is_zero(void)
{
	ASSERT_TRUE(parse_str("\nnot-a-number\n"));
	ASSERT_EQ_INT(0, (long long)g_park.last_updated_secs);
}

static void test_empty_payload(void)
{
	ASSERT_TRUE(!parkdata_parse(NULL, 0));
	ASSERT_TRUE(!parkdata_parse("", 0));
	ASSERT_EQ_INT(0, g_park.attraction_count);
	ASSERT_EQ_INT(0, g_park.show_count);
}

static void test_attractions(void)
{
	ASSERT_TRUE(parse_str(
		"\n1753776000\n"
		"A|abc12345|Space Mountain|O|45\n"
		"A|def00001|Haunted Mansion|C|\n"      /* empty wait → -1 */
		"A|ghi00002|Test Track|D|20\n"
		"A|jkl00003|River Ride|R|0\n"));
	ASSERT_EQ_INT(4, g_park.attraction_count);
	ASSERT_EQ_INT(0, g_park.show_count);

	ASSERT_EQ_STR("abc12345", g_park.attractions[0].id);
	ASSERT_EQ_STR("Space Mountain", g_park.attractions[0].name);
	ASSERT_EQ_INT('O', g_park.attractions[0].status);
	ASSERT_EQ_INT(45, g_park.attractions[0].wait);

	ASSERT_EQ_INT('C', g_park.attractions[1].status);
	ASSERT_EQ_INT(-1, g_park.attractions[1].wait);

	ASSERT_EQ_INT('D', g_park.attractions[2].status);
	ASSERT_EQ_INT(20, g_park.attractions[2].wait);

	ASSERT_EQ_INT('R', g_park.attractions[3].status);
	ASSERT_EQ_INT(0, g_park.attractions[3].wait);
}

static void test_shows(void)
{
	ASSERT_TRUE(parse_str(
		"\n1753776000\n"
		"S|show0001|Fireworks|O|9:30 PM,10:30 PM\n"
		"S|show0002|Quiet Show|C|\n"));
	ASSERT_EQ_INT(2, g_park.show_count);

	ASSERT_EQ_STR("show0001", g_park.shows[0].id);
	ASSERT_EQ_STR("Fireworks", g_park.shows[0].name);
	ASSERT_EQ_INT('O', g_park.shows[0].status);
	ASSERT_EQ_INT(2, g_park.shows[0].time_count);
	ASSERT_EQ_STR("9:30 PM", g_park.shows[0].times[0]);
	ASSERT_EQ_STR("10:30 PM", g_park.shows[0].times[1]);

	/* Empty times field → zero times, not a phantom empty slot. */
	ASSERT_EQ_INT(0, g_park.shows[1].time_count);
}

static void test_show_times_capped(void)
{
	ASSERT_TRUE(parse_str(
		"\n1753776000\n"
		"S|show0001|Parade|O|1 PM,2 PM,3 PM,4 PM,5 PM,6 PM\n"));
	ASSERT_EQ_INT(1, g_park.show_count);
	ASSERT_EQ_INT(MAX_TIMES_PER_SHOW, g_park.shows[0].time_count);
	ASSERT_EQ_STR("4 PM", g_park.shows[0].times[MAX_TIMES_PER_SHOW - 1]);
}

static void test_malformed_lines_skipped(void)
{
	/* None of these should crash or produce entries: missing fields,
	 * unknown type char, bare junk, too-short lines. */
	ASSERT_TRUE(parse_str(
		"\n1753776000\n"
		"garbage line with no pipes\n"
		"A\n"
		"A|onlyid\n"
		"A|id|name only\n"
		"A|id|name|O\n"           /* status but no wait field */
		"X|id|name|O|10\n"        /* unknown type */
		"\n"
		"A|good0001|Real Ride|O|5\n"));
	ASSERT_EQ_INT(1, g_park.attraction_count);
	ASSERT_EQ_STR("Real Ride", g_park.attractions[0].name);
	ASSERT_EQ_INT(0, g_park.show_count);
}

static void test_truncation_at_caps(void)
{
	/* Synthetic payload with more entities than the fixed arrays hold —
	 * parser must clamp, not overflow. */
	static char buf[16384];
	int off = snprintf(buf, sizeof(buf), "\n1753776000\n");
	for (int i = 0; i < MAX_ATTRACTIONS + 5; i++) {
		off += snprintf(buf + off, sizeof(buf) - off,
		                "A|id%06d|Ride %d|O|%d\n", i, i, i % 90);
	}
	for (int i = 0; i < MAX_SHOWS + 5; i++) {
		off += snprintf(buf + off, sizeof(buf) - off,
		                "S|sh%06d|Show %d|O|1 PM\n", i, i);
	}
	ASSERT_TRUE(off < (int)sizeof(buf));
	ASSERT_TRUE(parkdata_parse(buf, (uint16_t)off));
	ASSERT_EQ_INT(MAX_ATTRACTIONS, g_park.attraction_count);
	ASSERT_EQ_INT(MAX_SHOWS, g_park.show_count);
	/* Overflowing attractions must not have leaked into shows or
	 * clobbered earlier entries. */
	ASSERT_EQ_STR("Ride 0", g_park.attractions[0].name);
	ASSERT_EQ_STR("Show 0", g_park.shows[0].name);
}

static void test_future_days_parse(void)
{
	/* F| lines land in future_days regardless of position among the
	 * entity lines; PKJS includes today's date too (the publisher, not
	 * the parser, applies the dates-after-today convention). */
	ASSERT_TRUE(parse_str(
		"09:00|22:00\n1753776000\n"
		"F|2026-07-30|09:00|22:00\n"
		"A|aaaa|Ride|O|10\n"
		"F|2026-07-31|08:30|01:00\n"));
	ASSERT_EQ_INT(2, g_park.future_day_count);
	ASSERT_EQ_STR("2026-07-30", g_park.future_days[0].date);
	ASSERT_EQ_STR("09:00", g_park.future_days[0].open);
	ASSERT_EQ_STR("22:00", g_park.future_days[0].close);
	ASSERT_EQ_STR("2026-07-31", g_park.future_days[1].date);
	ASSERT_EQ_STR("08:30", g_park.future_days[1].open);
	ASSERT_EQ_STR("01:00", g_park.future_days[1].close);
	/* A|/S| parsing is unaffected by interleaved F| lines. */
	ASSERT_EQ_INT(1, g_park.attraction_count);
	ASSERT_EQ_STR("Ride", g_park.attractions[0].name);
}

static void test_future_days_capped(void)
{
	static char buf[1024];
	int off = snprintf(buf, sizeof(buf), "\n1753776000\n");
	for (int i = 0; i < MAX_FUTURE_DAYS + 3; i++) {
		off += snprintf(buf + off, sizeof(buf) - off,
		                "F|2026-08-%02d|09:00|22:00\n", i + 1);
	}
	ASSERT_TRUE(parkdata_parse(buf, (uint16_t)off));
	ASSERT_EQ_INT(MAX_FUTURE_DAYS, g_park.future_day_count);
	/* First seven kept, extras dropped — order preserved. */
	ASSERT_EQ_STR("2026-08-01", g_park.future_days[0].date);
	ASSERT_EQ_STR("2026-08-07",
	              g_park.future_days[MAX_FUTURE_DAYS - 1].date);
}

static void test_future_days_malformed_skipped(void)
{
	/* Bad shapes must be skipped without crashing or shifting the good
	 * entries: wrong date shape, missing fields, empty times. */
	ASSERT_TRUE(parse_str(
		"\n1753776000\n"
		"F|2026-7-30|09:00|22:00\n"     /* date not zero-padded (9 chars) */
		"F|2026/07/30|09:00|22:00\n"    /* wrong separators */
		"F|2026-13-01|09:00|22:00\n"    /* month out of range: parser only
		                                 * checks shape — see below */
		"F|2026-07-30|09:00\n"          /* missing close field */
		"F|2026-07-30\n"                /* date only */
		"F|\n"                          /* nothing at all */
		"F|2026-07-30||22:00\n"         /* empty open */
		"F|2026-07-30|09:00|\n"         /* empty close */
		"F|2026-07-31|10:00|21:00\n"    /* the one good line */
		"A|aaaa|Ride|O|5\n"));
	/* Shape-valid dates pass the parser (2026-13-01 is digits-and-dashes);
	 * the publisher's stricter month/day range check rejects them before
	 * any epoch math. So: the good line + the range-bad-but-shape-ok one. */
	ASSERT_EQ_INT(2, g_park.future_day_count);
	ASSERT_EQ_STR("2026-13-01", g_park.future_days[0].date);
	ASSERT_EQ_STR("2026-07-31", g_park.future_days[1].date);
	ASSERT_EQ_STR("10:00", g_park.future_days[1].open);
	ASSERT_EQ_INT(1, g_park.attraction_count);
}

static void test_future_days_cleared(void)
{
	ASSERT_TRUE(parse_str(
		"\n1753776000\nF|2026-07-30|09:00|22:00\n"));
	ASSERT_EQ_INT(1, g_park.future_day_count);
	parkdata_clear();
	ASSERT_EQ_INT(0, g_park.future_day_count);
	/* And a fresh parse without F| lines must not resurrect old ones. */
	ASSERT_TRUE(parse_str("\n1753776000\nA|aaaa|Ride|O|10\n"));
	ASSERT_EQ_INT(0, g_park.future_day_count);
}

static void test_clear_preserves_park_name(void)
{
	parkdata_set_park_name("Magic Kingdom");
	ASSERT_TRUE(parse_str("9 AM|10 PM\n1753776000\nA|aaaa|Ride|O|10\n"));
	parkdata_clear();
	/* Data gone, name kept — the loading screen shows it mid-refetch. */
	ASSERT_EQ_STR("Magic Kingdom", g_park.park_name);
	ASSERT_EQ_INT(0, g_park.attraction_count);
	ASSERT_EQ_INT(0, g_park.show_count);
	ASSERT_TRUE(!g_park.has_schedule);
	ASSERT_EQ_INT(0, (long long)g_park.last_updated_secs);
}

static void test_length_clamping(void)
{
	/* id longer than MAX_ID_LEN-1 and name longer than MAX_NAME_LEN-1
	 * are truncated with a terminating NUL, never overflowed. */
	ASSERT_TRUE(parse_str(
		"\n1753776000\n"
		"A|0123456789abcdef|"
		"This Attraction Name Is Far Longer Than Forty Characters Total|O|5\n"));
	ASSERT_EQ_INT(1, g_park.attraction_count);
	ASSERT_EQ_INT(MAX_ID_LEN - 1, (int)strlen(g_park.attractions[0].id));
	ASSERT_EQ_STR("0123456789a", g_park.attractions[0].id);
	ASSERT_EQ_INT(MAX_NAME_LEN - 1, (int)strlen(g_park.attractions[0].name));

	/* park_name has its own smaller buffer (24). */
	parkdata_set_park_name("A Very Long Park Name Indeed Beyond 24");
	ASSERT_EQ_INT(23, (int)strlen(g_park.park_name));
	parkdata_set_park_name(NULL);
	ASSERT_EQ_STR("", g_park.park_name);
}

static void test_all_closed(void)
{
	/* No data → "unknown", never "closed". */
	ASSERT_TRUE(parse_str("\n1753776000\n"));
	ASSERT_TRUE(!parkdata_all_closed());

	/* One operating ride keeps the park "open". */
	ASSERT_TRUE(parse_str(
		"\n1753776000\n"
		"A|aaaa|Closed Ride|C|\n"
		"A|bbbb|Open Ride|O|10\n"));
	ASSERT_TRUE(!parkdata_all_closed());

	/* Every non-operating status counts as closed. */
	ASSERT_TRUE(parse_str(
		"\n1753776000\n"
		"A|aaaa|Closed Ride|C|\n"
		"A|bbbb|Down Ride|D|\n"
		"A|cccc|Rehab Ride|R|\n"));
	ASSERT_TRUE(parkdata_all_closed());
}

int main(void)
{
	RUN_TEST(test_schedule_present);
	RUN_TEST(test_schedule_absent);
	RUN_TEST(test_schedule_no_pipe);
	RUN_TEST(test_timestamp_seconds_roundtrip);
	RUN_TEST(test_timestamp_garbage_is_zero);
	RUN_TEST(test_empty_payload);
	RUN_TEST(test_attractions);
	RUN_TEST(test_shows);
	RUN_TEST(test_show_times_capped);
	RUN_TEST(test_malformed_lines_skipped);
	RUN_TEST(test_truncation_at_caps);
	RUN_TEST(test_future_days_parse);
	RUN_TEST(test_future_days_capped);
	RUN_TEST(test_future_days_malformed_skipped);
	RUN_TEST(test_future_days_cleared);
	RUN_TEST(test_clear_preserves_park_name);
	RUN_TEST(test_length_clamping);
	RUN_TEST(test_all_closed);
	return unit_summary();
}
