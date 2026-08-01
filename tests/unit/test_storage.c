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
#define RAW_KEY_FAV_MIGRATED 6
#define RAW_KEY_FAV_LEGACY_BASE 1000
#define RAW_KEY_FAV_INDEX_BASE 2900
#define RAW_KEY_FAV2_BASE 3000

#define PARK0 (g_parks[0].id)
#define PARK1 (g_parks[1].id)

/* Arbitrary park UUIDs NOT in the g_parks fixture — favourites are
 * identity-keyed and must work for any park id (that's what makes them
 * survive uber destination switches, where g_parks is repopulated). */
#define DEST_A_PARK "aaaaaaaa-1111-4abc-8def-000000000001"
#define DEST_B_PARK "bbbbbbbb-2222-4abc-8def-000000000002"

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

	/* Favourites reads for a park with no favourites — any uuid at all
	 * — are empty, and reads never ALLOCATE an index entry. */
	ASSERT_EQ_INT(0, storage_get_favourites("no-such-park", FAV_ATTRACTIONS, favs));
	ASSERT_TRUE(!storage_is_favourite("no-such-park", FAV_ATTRACTIONS, "ride1"));
	int writes_after_reads = persist_stub_write_count();

	/* HOURS stay g_parks-gated (positional slots): set/get against an
	 * unknown park is a no-op, not a write into some other park's slot. */
	ASSERT_TRUE(!storage_get_park_hours("no-such-park", open, close));
	storage_set_park_hours("no-such-park", "9 AM", "10 PM");
	ASSERT_EQ_INT(writes_after_reads, persist_stub_write_count());

	/* NULL/empty park ids must not crash or write, favourites included
	 * (favourites accept any REAL uuid, but not a missing one). */
	ASSERT_EQ_INT(0, storage_get_favourites(NULL, FAV_ATTRACTIONS, favs));
	ASSERT_TRUE(!storage_is_favourite(NULL, FAV_ATTRACTIONS, "ride1"));
	storage_toggle_favourite(NULL, FAV_ATTRACTIONS, "ride1");
	storage_toggle_favourite("", FAV_ATTRACTIONS, "ride1");
	ASSERT_EQ_INT(writes_after_reads, persist_stub_write_count());
	ASSERT_TRUE(!storage_get_park_hours(NULL, open, close));

	/* An oversized park id can't round-trip through the fixed index
	 * field — refused, not truncated into a colliding key. */
	char longid[FAV_PARK_ID_LEN + 8];
	memset(longid, 'x', sizeof(longid) - 1);
	longid[sizeof(longid) - 1] = '\0';
	storage_toggle_favourite(longid, FAV_ATTRACTIONS, "ride1");
	ASSERT_EQ_INT(writes_after_reads, persist_stub_write_count());
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

static void test_glance_line_default(void)
{
	/* Never set → nothing to show; out buffers untouched by contract. */
	reset_all();
	char line[GLANCE_LINE_MAX];
	uint32_t exp = 0xdead;
	ASSERT_TRUE(!storage_get_glance_line(line, &exp));
	ASSERT_EQ_INT(0xdead, exp);
}

static void test_glance_line_roundtrip(void)
{
	reset_all();
	storage_set_glance_line("MK 45m · EP 30m", 1754081234u);
	char line[GLANCE_LINE_MAX];
	uint32_t exp = 0;
	ASSERT_TRUE(storage_get_glance_line(line, &exp));
	ASSERT_EQ_STR("MK 45m · EP 30m", line);
	ASSERT_EQ_INT(1754081234LL, (long long)exp);

	/* Overwrite replaces both text and expiry. */
	storage_set_glance_line("IA 60m", 42u);
	ASSERT_TRUE(storage_get_glance_line(line, &exp));
	ASSERT_EQ_STR("IA 60m", line);
	ASSERT_EQ_INT(42, (long long)exp);
}

static void test_glance_line_empty_set_clears(void)
{
	reset_all();
	storage_set_glance_line("MK 45m", 42u);
	storage_set_glance_line("", 99u);
	char line[GLANCE_LINE_MAX];
	ASSERT_TRUE(!storage_get_glance_line(line, NULL));
	/* Cleared in persist too, not just the RAM cache. */
	storage_init();
	ASSERT_TRUE(!storage_get_glance_line(line, NULL));
	/* NULL text clears the same way, and never crashes. */
	storage_set_glance_line("MK 45m", 42u);
	storage_set_glance_line(NULL, 0u);
	ASSERT_TRUE(!storage_get_glance_line(line, NULL));
}

static void test_glance_line_persists(void)
{
	/* Set must write through immediately — verified by re-arming the
	 * lazy cache (simulated relaunch) and re-reading from persist. */
	reset_all();
	storage_set_glance_line("MK 45m · EP 30m", 1754081234u);
	storage_init();   /* forget the RAM cache */
	char line[GLANCE_LINE_MAX];
	uint32_t exp = 0;
	ASSERT_TRUE(storage_get_glance_line(line, &exp));
	ASSERT_EQ_STR("MK 45m · EP 30m", line);
	ASSERT_EQ_INT(1754081234LL, (long long)exp);
}

static void test_glance_line_reads_stay_in_ram(void)
{
	/* Lazy cache contract, hit and miss both: one persist probe on the
	 * first query, then RAM only. */
	reset_all();
	storage_set_glance_line("MK 45m", 42u);
	storage_init();
	char line[GLANCE_LINE_MAX];
	storage_get_glance_line(line, NULL);   /* warm */
	int before = persist_stub_read_count();
	for (int i = 0; i < 50; i++) {
		ASSERT_TRUE(storage_get_glance_line(line, NULL));
	}
	ASSERT_EQ_INT(before, persist_stub_read_count());

	/* The miss is cached too. */
	storage_set_glance_line("", 0u);
	storage_get_glance_line(line, NULL);   /* warm the miss */
	before = persist_stub_read_count();
	for (int i = 0; i < 50; i++) {
		ASSERT_TRUE(!storage_get_glance_line(line, NULL));
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
	 * verified by re-arming all favourites RAM state (simulated
	 * relaunch) and re-reading. Covers the index entry AND the blob. */
	reset_all();
	storage_toggle_favourite(PARK0, FAV_SHOWS, "show1");
	storage_init();   /* forget index + fav caches */
	ASSERT_TRUE(storage_is_favourite(PARK0, FAV_SHOWS, "show1"));
}

/* --- identity-keyed favourites --------------------------------------- */

static void test_favs_arbitrary_uuid_not_in_gparks(void)
{
	/* The whole point of the re-key: favourites work for park uuids
	 * g_parks has never heard of (uber: parks of destinations other
	 * than the current one). */
	reset_all();
	storage_toggle_favourite(DEST_A_PARK, FAV_ATTRACTIONS, "ride1");
	ASSERT_TRUE(storage_is_favourite(DEST_A_PARK, FAV_ATTRACTIONS, "ride1"));
	char favs[MAX_FAVS_PER_LIST][FAV_ID_LEN];
	ASSERT_EQ_INT(1, storage_get_favourites(DEST_A_PARK, FAV_ATTRACTIONS, favs));
	ASSERT_EQ_STR("ride1", favs[0]);
	/* And it survives a relaunch. */
	storage_init();
	ASSERT_TRUE(storage_is_favourite(DEST_A_PARK, FAV_ATTRACTIONS, "ride1"));
}

static void test_favs_survive_destination_switch(void)
{
	/* Simulated uber flow: favourite parks in two "destinations", then
	 * switch (storage_on_parks_changed repopulates hours; g_parks
	 * membership is irrelevant to favourites) — everything survives. */
	reset_all();
	storage_toggle_favourite(DEST_A_PARK, FAV_ATTRACTIONS, "a-ride");
	storage_toggle_favourite(DEST_B_PARK, FAV_ATTRACTIONS, "b-ride");
	storage_toggle_favourite(DEST_B_PARK, FAV_SHOWS, "b-show");

	storage_on_parks_changed();   /* destination switch #1 */
	ASSERT_TRUE(storage_is_favourite(DEST_A_PARK, FAV_ATTRACTIONS, "a-ride"));
	ASSERT_TRUE(storage_is_favourite(DEST_B_PARK, FAV_ATTRACTIONS, "b-ride"));
	ASSERT_TRUE(storage_is_favourite(DEST_B_PARK, FAV_SHOWS, "b-show"));
	ASSERT_TRUE(!storage_is_favourite(DEST_A_PARK, FAV_SHOWS, "b-show"));

	storage_on_parks_changed();   /* and again */
	char favs[MAX_FAVS_PER_LIST][FAV_ID_LEN];
	ASSERT_EQ_INT(1, storage_get_favourites(DEST_A_PARK, FAV_ATTRACTIONS, favs));
	ASSERT_EQ_INT(1, storage_get_favourites(DEST_B_PARK, FAV_ATTRACTIONS, favs));
}

static void test_favs_budget_is_per_park(void)
{
	/* The old slot scheme made all destinations share one 8-entry
	 * budget per slot position; identity keys give every park its own
	 * 8 — filling park A must not evict anything of park B's. */
	reset_all();
	char id[32];
	for (int i = 0; i < MAX_FAVS_PER_LIST; i++) {
		snprintf(id, sizeof(id), "a-fav%d", i);
		storage_toggle_favourite(DEST_A_PARK, FAV_ATTRACTIONS, id);
	}
	storage_toggle_favourite(DEST_B_PARK, FAV_ATTRACTIONS, "b-fav");

	char favs[MAX_FAVS_PER_LIST][FAV_ID_LEN];
	ASSERT_EQ_INT(MAX_FAVS_PER_LIST,
	              storage_get_favourites(DEST_A_PARK, FAV_ATTRACTIONS, favs));
	ASSERT_EQ_INT(1, storage_get_favourites(DEST_B_PARK, FAV_ATTRACTIONS, favs));

	/* A's overflow replaces A's last entry — B untouched. */
	storage_toggle_favourite(DEST_A_PARK, FAV_ATTRACTIONS, "a-overflow");
	ASSERT_TRUE(storage_is_favourite(DEST_A_PARK, FAV_ATTRACTIONS, "a-overflow"));
	ASSERT_TRUE(storage_is_favourite(DEST_B_PARK, FAV_ATTRACTIONS, "b-fav"));
	ASSERT_EQ_INT(MAX_FAVS_PER_LIST,
	              storage_get_favourites(DEST_A_PARK, FAV_ATTRACTIONS, favs));
}

static void test_favs_table_full_lru_eviction(void)
{
	/* Fill all MAX_FAV_PARKS index entries, refresh the FIRST park's
	 * recency by reading it, then favourite a 13th park: the LRU entry
	 * (park 1, not park 0) is evicted and its favourites drop. */
	reset_all();
	char park[FAV_PARK_ID_LEN];
	for (int p = 0; p < MAX_FAV_PARKS; p++) {
		snprintf(park, sizeof(park), "lru-park-%02d", p);
		storage_toggle_favourite(park, FAV_ATTRACTIONS, "ride");
	}
	/* Touch park 0 (a read counts as use — the user is looking at it). */
	ASSERT_TRUE(storage_is_favourite("lru-park-00", FAV_ATTRACTIONS, "ride"));

	storage_toggle_favourite("lru-park-new", FAV_ATTRACTIONS, "ride");

	ASSERT_TRUE(storage_is_favourite("lru-park-new", FAV_ATTRACTIONS, "ride"));
	ASSERT_TRUE(storage_is_favourite("lru-park-00", FAV_ATTRACTIONS, "ride"));
	/* park 1 was least recently used — evicted, favourites gone. */
	ASSERT_TRUE(!storage_is_favourite("lru-park-01", FAV_ATTRACTIONS, "ride"));
	/* The rest survive. */
	ASSERT_TRUE(storage_is_favourite("lru-park-02", FAV_ATTRACTIONS, "ride"));
	ASSERT_TRUE(storage_is_favourite("lru-park-11", FAV_ATTRACTIONS, "ride"));

	/* Eviction reached persist too, not just RAM (relaunch check). */
	storage_init();
	ASSERT_TRUE(!storage_is_favourite("lru-park-01", FAV_ATTRACTIONS, "ride"));
	ASSERT_TRUE(storage_is_favourite("lru-park-new", FAV_ATTRACTIONS, "ride"));
}

/* Plant a legacy slot-keyed favourites blob (old key layout
 * 1000 + slot*2 + kind) the way the pre-identity code wrote them. */
static void plant_legacy_favs(int slot, int kind, const char *id0,
                              const char *id1)
{
	char blob[MAX_FAVS_PER_LIST][FAV_ID_LEN];
	memset(blob, 0, sizeof(blob));
	if (id0) snprintf(blob[0], FAV_ID_LEN, "%s", id0);
	if (id1) snprintf(blob[1], FAV_ID_LEN, "%s", id1);
	persist_write_data(RAW_KEY_FAV_LEGACY_BASE + slot * 2 + kind,
	                   blob, sizeof(blob));
}

static void test_favs_migration_from_legacy_keys(void)
{
	reset_all();
	plant_legacy_favs(0, FAV_ATTRACTIONS, "old-r1", "old-r2");
	plant_legacy_favs(1, FAV_SHOWS, "old-s1", NULL);

	/* First favourites access of the "launch" triggers the migration —
	 * counts preserved under the parks' identities per the current
	 * g_parks slot mapping. */
	ASSERT_TRUE(storage_is_favourite(PARK0, FAV_ATTRACTIONS, "old-r1"));
	char favs[MAX_FAVS_PER_LIST][FAV_ID_LEN];
	ASSERT_EQ_INT(2, storage_get_favourites(PARK0, FAV_ATTRACTIONS, favs));
	ASSERT_EQ_INT(1, storage_get_favourites(PARK1, FAV_SHOWS, favs));
	ASSERT_EQ_STR("old-s1", favs[0]);
	/* Kinds don't cross-pollinate. */
	ASSERT_EQ_INT(0, storage_get_favourites(PARK0, FAV_SHOWS, favs));

	/* Old keys are gone; the done marker is set. */
	ASSERT_TRUE(!persist_exists(RAW_KEY_FAV_LEGACY_BASE + 0));
	ASSERT_TRUE(!persist_exists(RAW_KEY_FAV_LEGACY_BASE + 1 * 2 + FAV_SHOWS));
	ASSERT_EQ_INT(1, persist_read_int(RAW_KEY_FAV_MIGRATED));

	/* Migrated favourites survive a relaunch (they're real new-key
	 * persist state, not RAM residue). */
	storage_init();
	ASSERT_TRUE(storage_is_favourite(PARK0, FAV_ATTRACTIONS, "old-r2"));
	ASSERT_TRUE(storage_is_favourite(PARK1, FAV_SHOWS, "old-s1"));
}

static void test_favs_migration_runs_once(void)
{
	/* The done marker — not the index's existence — gates the
	 * migration: once set, legacy-shaped keys are never read again,
	 * even across a relaunch. */
	reset_all();
	storage_toggle_favourite(PARK0, FAV_ATTRACTIONS, "ride1");   /* sets marker */
	ASSERT_EQ_INT(1, persist_read_int(RAW_KEY_FAV_MIGRATED));

	plant_legacy_favs(0, FAV_ATTRACTIONS, "ghost", NULL);
	storage_init();   /* relaunch */
	ASSERT_TRUE(!storage_is_favourite(PARK0, FAV_ATTRACTIONS, "ghost"));
	ASSERT_TRUE(storage_is_favourite(PARK0, FAV_ATTRACTIONS, "ride1"));
}

static void test_favs_migration_fresh_install(void)
{
	/* No legacy keys at all: first access just sets the marker and
	 * finds nothing. */
	reset_all();
	char favs[MAX_FAVS_PER_LIST][FAV_ID_LEN];
	ASSERT_EQ_INT(0, storage_get_favourites(PARK0, FAV_ATTRACTIONS, favs));
	ASSERT_EQ_INT(1, persist_read_int(RAW_KEY_FAV_MIGRATED));
}

static void test_favs_reads_stay_flat_across_parks(void)
{
	/* RAM-cache contract for the identity scheme: warm reads — same
	 * park AND a park with no favourites at all — never touch flash. */
	reset_all();
	storage_toggle_favourite(DEST_A_PARK, FAV_ATTRACTIONS, "ride1");
	storage_is_favourite(DEST_A_PARK, FAV_ATTRACTIONS, "ride1");   /* warm */
	storage_is_favourite(DEST_B_PARK, FAV_ATTRACTIONS, "ride1");   /* no entry */

	int before = persist_stub_read_count();
	for (int i = 0; i < 50; i++) {
		ASSERT_TRUE(storage_is_favourite(DEST_A_PARK, FAV_ATTRACTIONS, "ride1"));
		ASSERT_TRUE(!storage_is_favourite(DEST_B_PARK, FAV_ATTRACTIONS, "ride1"));
	}
	ASSERT_EQ_INT(before, persist_stub_read_count());
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
	RUN_TEST(test_glance_line_default);
	RUN_TEST(test_glance_line_roundtrip);
	RUN_TEST(test_glance_line_empty_set_clears);
	RUN_TEST(test_glance_line_persists);
	RUN_TEST(test_glance_line_reads_stay_in_ram);
	RUN_TEST(test_last_park);
	RUN_TEST(test_fav_queries_stay_in_ram);
	RUN_TEST(test_hours_queries_stay_in_ram);
	RUN_TEST(test_parks_changed_drops_hours_keeps_favs);
	RUN_TEST(test_toggle_writes_through);
	RUN_TEST(test_favs_arbitrary_uuid_not_in_gparks);
	RUN_TEST(test_favs_survive_destination_switch);
	RUN_TEST(test_favs_budget_is_per_park);
	RUN_TEST(test_favs_table_full_lru_eviction);
	RUN_TEST(test_favs_migration_from_legacy_keys);
	RUN_TEST(test_favs_migration_runs_once);
	RUN_TEST(test_favs_migration_fresh_install);
	RUN_TEST(test_favs_reads_stay_flat_across_parks);
	return unit_summary();
}
