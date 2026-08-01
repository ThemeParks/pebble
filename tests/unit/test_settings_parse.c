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

/* --- "line" segment (packed My Parks glance line) -------------------- */

static void test_line_valid(void)
{
	/* The real PKJS shape: UTF-8 middle dots, spaces, digits. */
	settings_msg_t m =
		parse_str("glance=1;line=MK 45m · EP 30m · IA 60m|1754081234");
	ASSERT_TRUE(m.has_glance);
	ASSERT_TRUE(m.has_line);
	ASSERT_EQ_STR("MK 45m · EP 30m · IA 60m", m.line_text);
	ASSERT_EQ_INT(1754081234, (long long)m.line_expiry);
}

static void test_line_alone(void)
{
	/* Line without a glance segment still parses (fields independent). */
	settings_msg_t m = parse_str("line=EPC 20m|99");
	ASSERT_TRUE(!m.has_glance);
	ASSERT_TRUE(m.has_line);
	ASSERT_EQ_STR("EPC 20m", m.line_text);
	ASSERT_EQ_INT(99, (long long)m.line_expiry);
}

static void test_line_missing(void)
{
	ASSERT_TRUE(!parse_str("glance=1").has_line);
	ASSERT_TRUE(!parse_str("").has_line);
}

static void test_line_empty_text_clears(void)
{
	/* "line=|0" is the explicit clear: has_line with empty text. */
	settings_msg_t m = parse_str("glance=1;line=|0");
	ASSERT_TRUE(m.has_line);
	ASSERT_EQ_STR("", m.line_text);
	ASSERT_EQ_INT(0, (long long)m.line_expiry);
}

static void test_line_malformed_expiry(void)
{
	ASSERT_TRUE(!parse_str("line=MK 45m|abc").has_line);   /* not digits */
	ASSERT_TRUE(!parse_str("line=MK 45m|12x4").has_line);  /* mixed */
	ASSERT_TRUE(!parse_str("line=MK 45m|").has_line);      /* empty */
	ASSERT_TRUE(!parse_str("line=MK 45m").has_line);       /* no pipe */
	ASSERT_TRUE(!parse_str("line=MK 45m|-5").has_line);    /* sign */
	/* 11 digits / over uint32: must not wrap into a future epoch. */
	ASSERT_TRUE(!parse_str("line=MK 45m|12345678901").has_line);
	ASSERT_TRUE(!parse_str("line=MK 45m|4294967296").has_line);
	/* uint32 max itself is fine. */
	settings_msg_t m = parse_str("line=MK 45m|4294967295");
	ASSERT_TRUE(m.has_line);
	ASSERT_EQ_INT(4294967295LL, (long long)m.line_expiry);
	/* A malformed line must not damage a well-formed neighbour. */
	m = parse_str("line=MK 45m|abc;glance=0");
	ASSERT_TRUE(!m.has_line);
	ASSERT_TRUE(m.has_glance);
}

static void test_line_splits_on_last_pipe(void)
{
	/* PKJS strips '|' from the text, but if one ever slipped through the
	 * LAST pipe still separates text from expiry. */
	settings_msg_t m = parse_str("line=A|B 5m|123");
	ASSERT_TRUE(m.has_line);
	ASSERT_EQ_STR("A|B 5m", m.line_text);
	ASSERT_EQ_INT(123, (long long)m.line_expiry);
}

static void test_line_oversize_rejected(void)
{
	/* Text at SETTINGS_LINE_MAX-1 bytes fits; one more is malformed
	 * (rejected whole, not truncated mid-UTF-8). */
	char buf[160];
	char text[SETTINGS_LINE_MAX + 2];
	memset(text, 'A', sizeof(text));

	text[SETTINGS_LINE_MAX - 1] = '\0';   /* 79 bytes of text */
	snprintf(buf, sizeof(buf), "line=%s|42", text);
	settings_msg_t m = parse_str(buf);
	ASSERT_TRUE(m.has_line);
	ASSERT_EQ_STR(text, m.line_text);

	memset(text, 'A', sizeof(text));
	text[SETTINGS_LINE_MAX] = '\0';       /* 80 bytes — one over */
	snprintf(buf, sizeof(buf), "line=%s|42", text);
	ASSERT_TRUE(!parse_str(buf).has_line);
}

static void test_line_key_must_match_exactly(void)
{
	ASSERT_TRUE(!parse_str("lines=MK 45m|42").has_line);
	ASSERT_TRUE(!parse_str("lin=MK 45m|42").has_line);
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
	RUN_TEST(test_line_valid);
	RUN_TEST(test_line_alone);
	RUN_TEST(test_line_missing);
	RUN_TEST(test_line_empty_text_clears);
	RUN_TEST(test_line_malformed_expiry);
	RUN_TEST(test_line_splits_on_last_pipe);
	RUN_TEST(test_line_oversize_rejected);
	RUN_TEST(test_line_key_must_match_exactly);
	return unit_summary();
}
