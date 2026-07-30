/*
 * test_settings_parse.c — host-side tests for the SETTINGS_DATA parser.
 *
 * Wire format (see settings_parse.h): semicolon-separated key=value
 * segments, currently just "glance=0|1". The parser's contract is that
 * garbage can never flip a field — only exact known key/value pairs set
 * the corresponding has_* gate.
 */
#include "settings_parse.h"
#include "unit.h"
#include <string.h>
#include <stdio.h>

static settings_msg_t parse_str(const char *raw)
{
	settings_msg_t out;
	settings_parse(raw, (uint16_t)strlen(raw), &out);
	return out;
}

static void test_glance_on(void)
{
	settings_msg_t m = parse_str("glance=1");
	ASSERT_TRUE(m.has_glance);
	ASSERT_TRUE(m.glance);
}

static void test_glance_off(void)
{
	settings_msg_t m = parse_str("glance=0");
	ASSERT_TRUE(m.has_glance);
	ASSERT_TRUE(!m.glance);
}

static void test_empty_and_null(void)
{
	settings_msg_t m = parse_str("");
	ASSERT_TRUE(!m.has_glance);

	/* NULL data must not crash and must yield a fully-cleared struct. */
	memset(&m, 0xff, sizeof(m));
	settings_parse(NULL, 5, &m);
	ASSERT_TRUE(!m.has_glance);
}

static void test_garbage(void)
{
	ASSERT_TRUE(!parse_str("total nonsense").has_glance);
	ASSERT_TRUE(!parse_str(";;;===;;;").has_glance);
	ASSERT_TRUE(!parse_str("=1").has_glance);          /* no key */
	ASSERT_TRUE(!parse_str("glance").has_glance);      /* no '=' */
	ASSERT_TRUE(!parse_str("glance=").has_glance);     /* empty value */
}

static void test_bad_values_do_not_flip(void)
{
	/* Anything other than exactly "0"/"1" leaves the field unset. */
	ASSERT_TRUE(!parse_str("glance=2").has_glance);
	ASSERT_TRUE(!parse_str("glance=10").has_glance);
	ASSERT_TRUE(!parse_str("glance=true").has_glance);
	ASSERT_TRUE(!parse_str("glance=01").has_glance);
}

static void test_unknown_keys_ignored(void)
{
	/* Future phone versions may append keys the watch doesn't know. */
	settings_msg_t m = parse_str("newthing=abc;glance=0;other=;x=1");
	ASSERT_TRUE(m.has_glance);
	ASSERT_TRUE(!m.glance);

	/* Key that merely prefixes/extends "glance" is NOT glance. */
	ASSERT_TRUE(!parse_str("glances=1").has_glance);
	ASSERT_TRUE(!parse_str("glanc=1").has_glance);
}

static void test_malformed_segments_dont_break_good_ones(void)
{
	settings_msg_t m = parse_str(";=;garbage;glance=1;=broken=");
	ASSERT_TRUE(m.has_glance);
	ASSERT_TRUE(m.glance);
}

static void test_len_bounds_respected(void)
{
	/* The buffer is not NUL-terminated at len — bytes past len must be
	 * invisible. "glance=1" truncated to "glance=" parses to nothing. */
	settings_msg_t m;
	settings_parse("glance=1", 7, &m);
	ASSERT_TRUE(!m.has_glance);
	settings_parse("glance=1;junk", 8, &m);
	ASSERT_TRUE(m.has_glance);
	ASSERT_TRUE(m.glance);
}

int main(void)
{
	RUN_TEST(test_glance_on);
	RUN_TEST(test_glance_off);
	RUN_TEST(test_empty_and_null);
	RUN_TEST(test_garbage);
	RUN_TEST(test_bad_values_do_not_flip);
	RUN_TEST(test_unknown_keys_ignored);
	RUN_TEST(test_malformed_segments_dont_break_good_ones);
	RUN_TEST(test_len_bounds_respected);
	return unit_summary();
}
