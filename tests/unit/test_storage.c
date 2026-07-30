/*
 * test_storage.c — host-side tests for the persist wrappers.
 *
 * Runs against the in-memory persist stub, so tests can also poke raw
 * keys (e.g. to plant an out-of-range sort mode) without a watch.
 */
#include "storage.h"
#include "config.h"
#include "persist_stub.h"
#include "unit.h"
#include <string.h>
#include <stdio.h>

/* Mirror storage.c's raw keys — needed to plant/inspect raw values. */
#define RAW_KEY_SORT_MODE 1
#define RAW_KEY_GLANCE_ENABLED 3

#define PARK0 (g_parks[0].id)
#define PARK1 (g_parks[1].id)

/* storage.c keeps RAM caches over persist; wiping the stub alone would
 * leave stale cache entries bleeding between tests. storage_init()
 * re-arms the lazy caches that aren't park-scoped (glance flag). */
static void reset_all(void)
{
	persist_stub_reset();
	storage_on_parks_changed();
	storage_init();
}

static void test_sort_mode_default(void)
{
	reset_all();
	ASSERT_EQ_INT(SORT_LONGEST, storage_get_sort_mode());
}

static void test_sort_mode_roundtrip(void)
{
	reset_all();
	storage_set_sort_mode(SORT_SHORTEST);
	ASSERT_EQ_INT(SORT_SHORTEST, storage_get_sort_mode());
	storage_set_sort_mode(SORT_AZ);
	ASSERT_EQ_INT(SORT_AZ, storage_get_sort_mode());
}

static void test_sort_mode_out_of_range(void)
{
	/* A stale/corrupt persisted value outside the enum must fall back
	 * to the default rather than index off the end of a mode table. */
	reset_all();
	persist_write_int(RAW_KEY_SORT_MODE, 42);
	ASSERT_EQ_INT(SORT_LONGEST, storage_get_sort_mode());
	persist_write_int(RAW_KEY_SORT_MODE, -3);
	ASSERT_EQ_INT(SORT_LONGEST, storage_get_sort_mode());
}

static void test_favourites_toggle(void)
{
	reset_all();
	ASSERT_TRUE(!storage_is_favourite(PARK0, FAV_ATTRACTIONS, "ride1"));

	storage_toggle_favourite(PARK0, FAV_ATTRACTIONS, "ride1");
	ASSERT_TRUE(storage_is_favourite(PARK0, FAV_ATTRACTIONS, "ride1"));

	char favs[MAX_FAVS_PER_LIST][FAV_ID_LEN];
	ASSERT_EQ_INT(1, storage_get_favourites(PARK0, FAV_ATTRACTIONS, favs));

	/* Toggling again removes it. */
	storage_toggle_favourite(PARK0, FAV_ATTRACTIONS, "ride1");
	ASSERT_TRUE(!storage_is_favourite(PARK0, FAV_ATTRACTIONS, "ride1"));
	ASSERT_EQ_INT(0, storage_get_favourites(PARK0, FAV_ATTRACTIONS, favs));
}

static void test_favourites_scoped_per_park_and_list(void)
{
	reset_all();
	storage_toggle_favourite(PARK0, FAV_ATTRACTIONS, "ride1");
	/* Same id: different park and different list kind stay independent. */
	ASSERT_TRUE(!storage_is_favourite(PARK1, FAV_ATTRACTIONS, "ride1"));
	ASSERT_TRUE(!storage_is_favourite(PARK0, FAV_SHOWS, "ride1"));
	ASSERT_TRUE(storage_is_favourite(PARK0, FAV_ATTRACTIONS, "ride1"));
}

static void test_favourites_full_replaces_last(void)
{
	reset_all();
	char id[32];   /* roomier than FAV_ID_LEN so snprintf never truncates */
	for (int i = 0; i < MAX_FAVS_PER_LIST; i++) {
		snprintf(id, sizeof(id), "fav%d", i);
		storage_toggle_favourite(PARK0, FAV_ATTRACTIONS, id);
	}
	char favs[MAX_FAVS_PER_LIST][FAV_ID_LEN];
	ASSERT_EQ_INT(MAX_FAVS_PER_LIST,
	              storage_get_favourites(PARK0, FAV_ATTRACTIONS, favs));

	/* Adding a 9th replaces the last slot instead of being dropped. */
	storage_toggle_favourite(PARK0, FAV_ATTRACTIONS, "overflow");
	ASSERT_TRUE(storage_is_favourite(PARK0, FAV_ATTRACTIONS, "overflow"));
	snprintf(id, sizeof(id), "fav%d", MAX_FAVS_PER_LIST - 1);
	ASSERT_TRUE(!storage_is_favourite(PARK0, FAV_ATTRACTIONS, id));
	ASSERT_TRUE(storage_is_favourite(PARK0, FAV_ATTRACTIONS, "fav0"));
	ASSERT_EQ_INT(MAX_FAVS_PER_LIST,
	              storage_get_favourites(PARK0, FAV_ATTRACTIONS, favs));
}

static void test_unknown_park_safe_defaults(void)
{
	reset_all();
	char favs[MAX_FAVS_PER_LIST][FAV_ID_LEN];
	char open[PARK_HOURS_STR_LEN], close[PARK_HOURS_STR_LEN];

	ASSERT_EQ_INT(0, storage_get_favourites("no-such-park", FAV_ATTRACTIONS, favs));
	ASSERT_TRUE(!storage_is_favourite("no-such-park", FAV_ATTRACTIONS, "ride1"));
	ASSERT_TRUE(!storage_get_park_hours("no-such-park", open, close));

	/* Toggle/set against an unknown park must be a no-op, not a write
	 * into some other park's slot. */
	storage_toggle_favourite("no-such-park", FAV_ATTRACTIONS, "ride1");
	storage_set_park_hours("no-such-park", "9 AM", "10 PM");
	ASSERT_EQ_INT(0, persist_stub_write_count());
	ASSERT_TRUE(!storage_is_favourite(PARK0, FAV_ATTRACTIONS, "ride1"));

	/* NULL park id must not crash either. */
	ASSERT_EQ_INT(0, storage_get_favourites(NULL, FAV_ATTRACTIONS, favs));
	ASSERT_TRUE(!storage_get_park_hours(NULL, open, close));
}

static void test_park_hours_roundtrip(void)
{
	reset_all();
	char open[PARK_HOURS_STR_LEN], close[PARK_HOURS_STR_LEN];

	/* Missing → false, buffers untouched by contract. */
	ASSERT_TRUE(!storage_get_park_hours(PARK0, open, close));

	storage_set_park_hours(PARK0, "9:00 AM", "10:00 PM");
	ASSERT_TRUE(storage_get_park_hours(PARK0, open, close));
	ASSERT_EQ_STR("9:00 AM", open);
	ASSERT_EQ_STR("10:00 PM", close);

	/* Second park has its own slot. */
	storage_set_park_hours(PARK1, "8 AM", "9 PM");
	ASSERT_TRUE(storage_get_park_hours(PARK1, open, close));
	ASSERT_EQ_STR("8 AM", open);
	ASSERT_EQ_STR("9 PM", close);
	ASSERT_TRUE(storage_get_park_hours(PARK0, open, close));
	ASSERT_EQ_STR("9:00 AM", open);
}

static void test_glance_enabled_default(void)
{
	/* Never set → enabled: the glance predates the toggle, so upgrading
	 * users keep the behaviour they had. */
	reset_all();
	ASSERT_TRUE(storage_get_glance_enabled());
}

static void test_glance_enabled_roundtrip(void)
{
	reset_all();
	storage_set_glance_enabled(false);
	ASSERT_TRUE(!storage_get_glance_enabled());
	storage_set_glance_enabled(true);
	ASSERT_TRUE(storage_get_glance_enabled());
}

static void test_glance_enabled_persists(void)
{
	/* The set must write through immediately — verified by re-arming the
	 * lazy cache (simulated relaunch) and re-reading from persist. */
	reset_all();
	storage_set_glance_enabled(false);
	storage_init();   /* forget the RAM cache */
	ASSERT_TRUE(!storage_get_glance_enabled());

	/* Raw persisted value survives too, and nonzero reads as enabled. */
	ASSERT_EQ_INT(0, persist_read_int(RAW_KEY_GLANCE_ENABLED));
	persist_write_int(RAW_KEY_GLANCE_ENABLED, 7);
	storage_init();
	ASSERT_TRUE(storage_get_glance_enabled());
}

static void test_glance_reads_stay_in_ram(void)
{
	/* Lazy cache contract: one persist read on first query, then RAM. */
	reset_all();
	storage_set_glance_enabled(false);
	storage_init();
	storage_get_glance_enabled();   /* warm */
	int before = persist_stub_read_count();
	for (int i = 0; i < 50; i++) {
		storage_get_glance_enabled();
	}
	ASSERT_EQ_INT(before, persist_stub_read_count());
}

static void test_last_park(void)
{
	reset_all();
	ASSERT_TRUE(storage_get_last_park() == NULL);
	storage_set_last_park(g_parks[2].id);
	ASSERT_EQ_STR(g_parks[2].id, storage_get_last_park());
	/* NULL is ignored, previous value survives. */
	storage_set_last_park(NULL);
	ASSERT_EQ_STR(g_parks[2].id, storage_get_last_park());
}

static void test_fav_queries_stay_in_ram(void)
{
	/* The park view calls storage_is_favourite per row per frame during
	 * scroll animations — after the first load those must be pure RAM
	 * scans, not flash reads. */
	reset_all();
	storage_toggle_favourite(PARK0, FAV_ATTRACTIONS, "ride1");
	storage_is_favourite(PARK0, FAV_ATTRACTIONS, "ride1");   /* warm */

	int before = persist_stub_read_count();
	for (int i = 0; i < 50; i++) {
		storage_is_favourite(PARK0, FAV_ATTRACTIONS, "ride1");
		storage_is_favourite(PARK0, FAV_ATTRACTIONS, "other");
	}
	ASSERT_EQ_INT(before, persist_stub_read_count());

	/* Switching parks reloads once, then stays in RAM again. */
	storage_is_favourite(PARK1, FAV_ATTRACTIONS, "ride1");
	int after_switch = persist_stub_read_count();
	for (int i = 0; i < 50; i++) {
		storage_is_favourite(PARK1, FAV_ATTRACTIONS, "ride1");
	}
	ASSERT_EQ_INT(after_switch, persist_stub_read_count());
}

static void test_hours_queries_stay_in_ram(void)
{
	/* Same contract for the park picker's per-row hours lookup —
	 * including the negative case (park with no stored hours). */
	reset_all();
	char open[PARK_HOURS_STR_LEN], close[PARK_HOURS_STR_LEN];
	storage_set_park_hours(PARK0, "09:00", "22:00");
	storage_get_park_hours(PARK0, open, close);   /* warm */
	storage_get_park_hours(PARK1, open, close);   /* warm the miss too */

	int before = persist_stub_read_count();
	for (int i = 0; i < 50; i++) {
		ASSERT_TRUE(storage_get_park_hours(PARK0, open, close));
		ASSERT_TRUE(!storage_get_park_hours(PARK1, open, close));
	}
	ASSERT_EQ_INT(before, persist_stub_read_count());
	ASSERT_EQ_STR("09:00", open);
}

static void test_parks_changed_drops_hours_keeps_favs(void)
{
	/* Uber destination switch: hours are positional (slot N = Nth park
	 * of the current destination) so they're wiped, persisted included.
	 * Favourites persist survives — only its RAM cache drops. */
	reset_all();
	storage_set_park_hours(PARK0, "09:00", "22:00");
	storage_toggle_favourite(PARK0, FAV_ATTRACTIONS, "ride1");

	storage_on_parks_changed();

	char open[PARK_HOURS_STR_LEN], close[PARK_HOURS_STR_LEN];
	ASSERT_TRUE(!storage_get_park_hours(PARK0, open, close));
	ASSERT_TRUE(storage_is_favourite(PARK0, FAV_ATTRACTIONS, "ride1"));
}

static void test_toggle_writes_through(void)
{
	/* A toggle must land in persist immediately (not just the cache) —
	 * verified by dropping the cache and re-reading. */
	reset_all();
	storage_toggle_favourite(PARK0, FAV_SHOWS, "show1");
	storage_on_parks_changed();   /* drop RAM cache */
	ASSERT_TRUE(storage_is_favourite(PARK0, FAV_SHOWS, "show1"));
}

int main(void)
{
	RUN_TEST(test_sort_mode_default);
	RUN_TEST(test_sort_mode_roundtrip);
	RUN_TEST(test_sort_mode_out_of_range);
	RUN_TEST(test_favourites_toggle);
	RUN_TEST(test_favourites_scoped_per_park_and_list);
	RUN_TEST(test_favourites_full_replaces_last);
	RUN_TEST(test_unknown_park_safe_defaults);
	RUN_TEST(test_park_hours_roundtrip);
	RUN_TEST(test_glance_enabled_default);
	RUN_TEST(test_glance_enabled_roundtrip);
	RUN_TEST(test_glance_enabled_persists);
	RUN_TEST(test_glance_reads_stay_in_ram);
	RUN_TEST(test_last_park);
	RUN_TEST(test_fav_queries_stay_in_ram);
	RUN_TEST(test_hours_queries_stay_in_ram);
	RUN_TEST(test_parks_changed_drops_hours_keeps_favs);
	RUN_TEST(test_toggle_writes_through);
	return unit_summary();
}
