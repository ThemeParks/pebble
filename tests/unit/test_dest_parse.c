/*
 * test_dest_parse.c — host-side tests for the destination-list parser.
 *
 * Wire format (see dest_parse.h): X|nogps marker, H|Section headers,
 * D|id|name|parkCount|distKm rows.
 */
#include "dest_parse.h"
#include "unit.h"
#include <string.h>
#include <stdio.h>

static void parse_str(const char *raw)
{
	dest_parse(raw, (uint16_t)strlen(raw));
}

static void test_basic_sections(void)
{
	parse_str(
		"H|Nearby\n"
		"D|dest-wdw|Walt Disney World|4|12\n"
		"D|dest-uni|Universal Orlando|3|18\n"
		"H|Europe\n"
		"D|dest-dlp|Disneyland Paris|2|0\n");

	ASSERT_EQ_INT(3, g_dest_count);
	ASSERT_EQ_INT(2, g_dest_section_count);
	ASSERT_EQ_INT(0, g_dest_nearby_section);
	ASSERT_TRUE(!g_dest_no_location);

	ASSERT_EQ_STR("Nearby", g_dest_sections[0].name);
	ASSERT_EQ_INT(0, g_dest_sections[0].first_row);
	ASSERT_EQ_INT(2, g_dest_sections[0].count);
	ASSERT_EQ_STR("Europe", g_dest_sections[1].name);
	ASSERT_EQ_INT(2, g_dest_sections[1].first_row);
	ASSERT_EQ_INT(1, g_dest_sections[1].count);

	ASSERT_EQ_STR("dest-wdw", g_dests[0].id);
	ASSERT_EQ_STR("Walt Disney World", g_dests[0].name);
	ASSERT_EQ_INT(4, g_dests[0].park_count);
	ASSERT_EQ_INT(12, g_dests[0].dist_km);
	ASSERT_EQ_INT(0, g_dests[0].section);

	ASSERT_EQ_INT(0, g_dests[2].dist_km);
	ASSERT_EQ_INT(1, g_dests[2].section);
}

static void test_no_location_marker(void)
{
	parse_str(
		"X|nogps\n"
		"H|Europe\n"
		"D|dest-dlp|Disneyland Paris|2|0\n");
	ASSERT_TRUE(g_dest_no_location);
	ASSERT_EQ_INT(1, g_dest_count);
	ASSERT_EQ_INT(1, g_dest_section_count);
	ASSERT_EQ_INT(-1, g_dest_nearby_section);

	/* A later payload with a location fix clears the flag. */
	parse_str("H|Nearby\nD|dest-wdw|Walt Disney World|4|5\n");
	ASSERT_TRUE(!g_dest_no_location);
	ASSERT_EQ_INT(0, g_dest_nearby_section);
}

static void test_missing_dist_field(void)
{
	/* Three-field rows (no distKm) parse with dist 0. */
	parse_str("H|Europe\nD|dest-x|Some Park|1\n");
	ASSERT_EQ_INT(1, g_dest_count);
	ASSERT_EQ_INT(0, g_dests[0].dist_km);
	ASSERT_EQ_INT(1, g_dests[0].park_count);
}

static void test_malformed_lines_skipped(void)
{
	parse_str(
		"garbage\n"
		"D|\n"
		"D|only-id\n"
		"Q|unknown|type|1|2\n"
		"\n"
		"H|Region\n"
		"D|good|Good Dest|2|0\n");
	ASSERT_EQ_INT(1, g_dest_count);
	ASSERT_EQ_STR("Good Dest", g_dests[0].name);
	ASSERT_EQ_INT(1, g_dest_section_count);
}

static void test_empty_payload(void)
{
	parse_str("");
	ASSERT_EQ_INT(0, g_dest_count);
	ASSERT_EQ_INT(0, g_dest_section_count);
	ASSERT_EQ_INT(-1, g_dest_nearby_section);
	ASSERT_TRUE(!g_dest_no_location);
}

static void test_name_and_id_truncation(void)
{
	parse_str(
		"H|Region\n"
		"D|an-extremely-long-destination-id-that-overflows-forty-chars"
		"|A Destination Name Well Beyond The Thirty-Two Limit|5|3\n");
	ASSERT_EQ_INT(1, g_dest_count);
	ASSERT_EQ_INT(MAX_DEST_ID - 1, (int)strlen(g_dests[0].id));
	ASSERT_EQ_INT(MAX_DEST_NAME - 1, (int)strlen(g_dests[0].name));
	ASSERT_EQ_INT(5, g_dests[0].park_count);
	ASSERT_EQ_INT(3, g_dests[0].dist_km);
}

static void test_caps(void)
{
	/* More destinations than MAX_DESTS — parser must clamp cleanly. */
	static char buf[32768];
	int off = snprintf(buf, sizeof(buf), "H|Region\n");
	for (int i = 0; i < MAX_DESTS + 10; i++) {
		off += snprintf(buf + off, sizeof(buf) - off,
		                "D|id%06d|Dest %d|1|0\n", i, i);
	}
	ASSERT_TRUE(off < (int)sizeof(buf));
	dest_parse(buf, (uint16_t)off);
	ASSERT_EQ_INT(MAX_DESTS, g_dest_count);
	ASSERT_EQ_STR("Dest 0", g_dests[0].name);

	/* More sections than MAX_SECTIONS — extra headers are dropped but
	 * their rows still land in the last accepted section's run. */
	off = 0;
	for (int i = 0; i < MAX_SECTIONS + 3; i++) {
		off += snprintf(buf + off, sizeof(buf) - off,
		                "H|Region %d\nD|sid%03d|SDest %d|1|0\n", i, i, i);
	}
	dest_parse(buf, (uint16_t)off);
	ASSERT_EQ_INT(MAX_SECTIONS, g_dest_section_count);
	ASSERT_EQ_INT(MAX_SECTIONS + 3, g_dest_count);
}

static void test_fixed_marker(void)
{
	uint16_t off = 0;

	/* Marked payload: true, offset lands on the first park row. */
	const char *marked = "M|fixed\nid1|Efteling|circle|pink\n";
	ASSERT_TRUE(dest_parks_fixed_marker(marked, (uint16_t)strlen(marked), &off));
	ASSERT_EQ_INT(8, off);
	ASSERT_TRUE(strncmp(marked + off, "id1|", 4) == 0);

	/* Marker-only payload (degenerate but well-formed). */
	ASSERT_TRUE(dest_parks_fixed_marker("M|fixed", 7, &off));
	ASSERT_EQ_INT(7, off);

	/* Unmarked payload: false, offset 0. */
	const char *plain = "id1|Efteling|circle|pink\n";
	ASSERT_TRUE(!dest_parks_fixed_marker(plain, (uint16_t)strlen(plain), &off));
	ASSERT_EQ_INT(0, off);

	/* Near-misses must not match: prefix without line break, park id
	 * that happens to start with M|, short/empty payloads. */
	ASSERT_TRUE(!dest_parks_fixed_marker("M|fixedX|name|i|c\n", 18, &off));
	ASSERT_EQ_INT(0, off);
	ASSERT_TRUE(!dest_parks_fixed_marker("M|fix", 5, &off));
	ASSERT_TRUE(!dest_parks_fixed_marker("", 0, &off));
	ASSERT_TRUE(!dest_parks_fixed_marker(NULL, 10, &off));
}

int main(void)
{
	RUN_TEST(test_basic_sections);
	RUN_TEST(test_no_location_marker);
	RUN_TEST(test_missing_dist_field);
	RUN_TEST(test_empty_payload);
	RUN_TEST(test_malformed_lines_skipped);
	RUN_TEST(test_name_and_id_truncation);
	RUN_TEST(test_caps);
	RUN_TEST(test_fixed_marker);
	return unit_summary();
}
