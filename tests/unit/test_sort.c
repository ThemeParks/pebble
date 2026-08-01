/*
 * test_sort.c — host-side tests for the index sorts.
 *
 * g_park is populated directly (no wire payloads — that's parkdata's
 * test) and favourites go through the real storage layer backed by the
 * in-memory persist stub, so the fav-pinning path is the production one.
 */
#include "sort.h"
#include "config.h"
#include "persist_stub.h"
#include "unit.h"
#include <string.h>
#include <stdio.h>

/* Any id from the test_config fixture works; slot math is what matters. */
#define PARK (g_parks[0].id)

static void reset(void)
{
	persist_stub_reset();
	/* storage.c caches favourites (and their identity index) in RAM —
	 * re-arm via storage_init (the simulated-relaunch hook) or
	 * favourites toggled by a previous test bleed into this one. */
	storage_init();
	parkdata_clear();
}

static void add_attr(const char *id, const char *name, char status, int wait)
{
	attraction_t *a = &g_park.attractions[g_park.attraction_count++];
	snprintf(a->id, MAX_ID_LEN, "%s", id);
	snprintf(a->name, MAX_NAME_LEN, "%s", name);
	a->status = status;
	a->wait = wait;
}

static void add_show(const char *id, const char *name, int time_count)
{
	show_t *s = &g_park.shows[g_park.show_count++];
	snprintf(s->id, MAX_ID_LEN, "%s", id);
	snprintf(s->name, MAX_NAME_LEN, "%s", name);
	s->status = 'O';
	s->time_count = (uint8_t)time_count;
	for (int i = 0; i < time_count; i++) {
		/* Content is irrelevant to sorting — only time_count matters. */
		snprintf(s->times[i], MAX_TIME_LEN, "9 PM");
	}
}

/* Convenience: id of the attraction at sorted position i. */
static const char *attr_at(const uint8_t *out, int i)
{
	return g_park.attractions[out[i]].id;
}

static void test_bucket_order(void)
{
	reset();
	add_attr("plainclosed", "Plain Closed", 'C', -1);
	add_attr("plainop", "Plain Operating", 'O', 20);
	add_attr("favclosed", "Fav Closed", 'D', -1);
	add_attr("favop", "Fav Operating", 'O', 10);
	storage_toggle_favourite(PARK, FAV_ATTRACTIONS, "favclosed");
	storage_toggle_favourite(PARK, FAV_ATTRACTIONS, "favop");

	uint8_t out[MAX_ATTRACTIONS];
	uint8_t n = sort_attractions(PARK, SORT_AZ, out);
	ASSERT_EQ_INT(4, n);
	/* Bucket order: fav+operating, fav, operating, everything else. */
	ASSERT_EQ_STR("favop", attr_at(out, 0));
	ASSERT_EQ_STR("favclosed", attr_at(out, 1));
	ASSERT_EQ_STR("plainop", attr_at(out, 2));
	ASSERT_EQ_STR("plainclosed", attr_at(out, 3));
}

static void test_longest_descending(void)
{
	reset();
	add_attr("r10", "Ten", 'O', 10);
	add_attr("r50", "Fifty", 'O', 50);
	add_attr("runk", "Unknown", 'O', -1);
	add_attr("r30", "Thirty", 'O', 30);

	uint8_t out[MAX_ATTRACTIONS];
	uint8_t n = sort_attractions(PARK, SORT_LONGEST, out);
	ASSERT_EQ_INT(4, n);
	ASSERT_EQ_STR("r50", attr_at(out, 0));
	ASSERT_EQ_STR("r30", attr_at(out, 1));
	ASSERT_EQ_STR("r10", attr_at(out, 2));
	ASSERT_EQ_STR("runk", attr_at(out, 3));
}

static void test_shortest_unknown_last(void)
{
	reset();
	add_attr("runk", "Unknown", 'O', -1);
	add_attr("r30", "Thirty", 'O', 30);
	add_attr("r0", "Zero", 'O', 0);
	add_attr("r10", "Ten", 'O', 10);

	uint8_t out[MAX_ATTRACTIONS];
	uint8_t n = sort_attractions(PARK, SORT_SHORTEST, out);
	ASSERT_EQ_INT(4, n);
	ASSERT_EQ_STR("r0", attr_at(out, 0));
	ASSERT_EQ_STR("r10", attr_at(out, 1));
	ASSERT_EQ_STR("r30", attr_at(out, 2));
	/* -1 (null wait) is treated as +infinity in SHORTEST mode. */
	ASSERT_EQ_STR("runk", attr_at(out, 3));
}

static void test_az_alphabetical(void)
{
	reset();
	add_attr("rz", "Zebra Coaster", 'O', 5);
	add_attr("ra", "Apple Drop", 'O', 60);
	add_attr("rm", "Mango Spin", 'O', 30);

	uint8_t out[MAX_ATTRACTIONS];
	uint8_t n = sort_attractions(PARK, SORT_AZ, out);
	ASSERT_EQ_INT(3, n);
	ASSERT_EQ_STR("ra", attr_at(out, 0));
	ASSERT_EQ_STR("rm", attr_at(out, 1));
	ASSERT_EQ_STR("rz", attr_at(out, 2));
}

static void test_favourites_pinned_all_modes(void)
{
	/* A favourite — even a closed one — must outrank a non-favourite
	 * with the "best" value in every sort mode. */
	sort_mode_t modes[] = { SORT_LONGEST, SORT_SHORTEST, SORT_AZ };
	for (int m = 0; m < 3; m++) {
		reset();
		add_attr("aaa", "AAA Monster Wait", 'O', 300);
		add_attr("fav", "ZZZ Favourite", 'C', -1);
		storage_toggle_favourite(PARK, FAV_ATTRACTIONS, "fav");

		uint8_t out[MAX_ATTRACTIONS];
		uint8_t n = sort_attractions(PARK, modes[m], out);
		ASSERT_EQ_INT(2, n);
		ASSERT_EQ_STR("fav", attr_at(out, 0));
		ASSERT_EQ_STR("aaa", attr_at(out, 1));
	}
}

static void test_sort_shows(void)
{
	reset();
	add_show("s1", "First Show", 2);
	add_show("s2", "Fav But Done", 0);   /* no times → filtered out */
	add_show("s3", "Fav Upcoming", 1);
	add_show("s4", "Last Show", 3);
	storage_toggle_favourite(PARK, FAV_SHOWS, "s2");
	storage_toggle_favourite(PARK, FAV_SHOWS, "s3");

	uint8_t out[MAX_SHOWS];
	uint8_t n = sort_shows(PARK, out);
	ASSERT_EQ_INT(3, n);
	/* Favourite with times first; the rest keep arrival order. */
	ASSERT_EQ_STR("s3", g_park.shows[out[0]].id);
	ASSERT_EQ_STR("s1", g_park.shows[out[1]].id);
	ASSERT_EQ_STR("s4", g_park.shows[out[2]].id);
}

static void test_show_favourites_separate_from_attractions(void)
{
	/* An attraction favourite with the same id must not pin a show. */
	reset();
	add_show("other", "Other Show", 1);
	add_show("dup", "Dup Show", 1);
	storage_toggle_favourite(PARK, FAV_ATTRACTIONS, "dup");

	uint8_t out[MAX_SHOWS];
	uint8_t n = sort_shows(PARK, out);
	ASSERT_EQ_INT(2, n);
	/* If attraction favs leaked into shows, "dup" would jump to front. */
	ASSERT_EQ_STR("other", g_park.shows[out[0]].id);
	ASSERT_EQ_STR("dup", g_park.shows[out[1]].id);
}

static void test_find_id_attractions(void)
{
	/* The background-refresh cursor restore: after a re-sort, the same
	 * id must be findable at its new position. */
	reset();
	add_attr("r10", "Ten", 'O', 10);
	add_attr("r50", "Fifty", 'O', 50);
	add_attr("r30", "Thirty", 'O', 30);

	uint8_t out[MAX_ATTRACTIONS];
	uint8_t n = sort_attractions(PARK, SORT_LONGEST, out);
	ASSERT_EQ_INT(3, n);
	/* LONGEST order: r50, r30, r10 */
	ASSERT_EQ_INT(0, sort_find_id(out, n, FAV_ATTRACTIONS, "r50"));
	ASSERT_EQ_INT(1, sort_find_id(out, n, FAV_ATTRACTIONS, "r30"));
	ASSERT_EQ_INT(2, sort_find_id(out, n, FAV_ATTRACTIONS, "r10"));

	/* Re-sort with changed waits — same id, new position. */
	g_park.attractions[0].wait = 90;   /* r10 now longest */
	n = sort_attractions(PARK, SORT_LONGEST, out);
	ASSERT_EQ_INT(0, sort_find_id(out, n, FAV_ATTRACTIONS, "r10"));
}

static void test_find_id_shows(void)
{
	reset();
	add_show("s1", "First Show", 2);
	add_show("s2", "Second Show", 1);

	uint8_t out[MAX_SHOWS];
	uint8_t n = sort_shows(PARK, out);
	ASSERT_EQ_INT(2, n);
	ASSERT_EQ_INT(1, sort_find_id(out, n, FAV_SHOWS, "s2"));
}

static void test_find_id_absent_and_degenerate(void)
{
	reset();
	add_attr("r10", "Ten", 'O', 10);

	uint8_t out[MAX_ATTRACTIONS];
	uint8_t n = sort_attractions(PARK, SORT_LONGEST, out);
	ASSERT_EQ_INT(-1, sort_find_id(out, n, FAV_ATTRACTIONS, "gone"));
	ASSERT_EQ_INT(-1, sort_find_id(out, 0, FAV_ATTRACTIONS, "r10"));
	ASSERT_EQ_INT(-1, sort_find_id(out, n, FAV_ATTRACTIONS, ""));
	ASSERT_EQ_INT(-1, sort_find_id(out, n, FAV_ATTRACTIONS, NULL));
	ASSERT_EQ_INT(-1, sort_find_id(NULL, n, FAV_ATTRACTIONS, "r10"));
}

int main(void)
{
	RUN_TEST(test_bucket_order);
	RUN_TEST(test_longest_descending);
	RUN_TEST(test_shortest_unknown_last);
	RUN_TEST(test_az_alphabetical);
	RUN_TEST(test_favourites_pinned_all_modes);
	RUN_TEST(test_sort_shows);
	RUN_TEST(test_show_favourites_separate_from_attractions);
	RUN_TEST(test_find_id_attractions);
	RUN_TEST(test_find_id_shows);
	RUN_TEST(test_find_id_absent_and_degenerate);
	return unit_summary();
}
