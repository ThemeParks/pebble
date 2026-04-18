/*
 * sort.c — index-based sorting with favourites pinned to top.
 *
 * No data copies, no per-call allocations. Just walks g_park.attractions /
 * g_park.shows, builds an index array, and bubble-sorts within buckets.
 * N is small (max 40 attractions, 20 shows), so simple is fine.
 */

#include "sort.h"
#include "config.h"
#include <string.h>

/* Module-locals consulted by the sort comparators. Set before sorting. */
static char s_favs[MAX_FAVS_PER_LIST][FAV_ID_LEN];
static int s_fav_count;
static sort_mode_t s_mode;

static bool is_fav(const char *id)
{
	if (!id || !id[0]) return false;
	for (int i = 0; i < MAX_FAVS_PER_LIST; i++) {
		if (s_favs[i][0] && strcmp(s_favs[i], id) == 0) return true;
	}
	return false;
}

static int attr_bucket(uint8_t idx)
{
	const attraction_t *a = &g_park.attractions[idx];
	bool fav = is_fav(a->id);
	bool op = (a->status == 'O');
	if (fav && op) return 0;
	if (fav) return 1;
	if (op) return 2;
	return 3;
}

/* Returns <0 if a should come before b. */
static int attr_compare(uint8_t ai, uint8_t bi)
{
	int ba = attr_bucket(ai);
	int bb = attr_bucket(bi);
	if (ba != bb) return ba - bb;

	const attraction_t *a = &g_park.attractions[ai];
	const attraction_t *b = &g_park.attractions[bi];

	if (s_mode == SORT_LONGEST) {
		int aw = (a->wait < 0) ? -1 : a->wait;
		int bw = (b->wait < 0) ? -1 : b->wait;
		return bw - aw;          /* descending */
	} else if (s_mode == SORT_SHORTEST) {
		/* Treat null as +infinity so it sorts last */
		int aw = (a->wait < 0) ? 99999 : a->wait;
		int bw = (b->wait < 0) ? 99999 : b->wait;
		return aw - bw;          /* ascending */
	}
	return strcmp(a->name, b->name);
}

/* Simple insertion sort — N <= 40, this is plenty fast. */
static void insertion_sort_attr(uint8_t *arr, uint8_t n)
{
	for (uint8_t i = 1; i < n; i++) {
		uint8_t key = arr[i];
		int j = i - 1;
		while (j >= 0 && attr_compare(arr[j], key) > 0) {
			arr[j + 1] = arr[j];
			j--;
		}
		arr[j + 1] = key;
	}
}

uint8_t sort_attractions(const char *park_id, sort_mode_t mode,
                         uint8_t out[MAX_ATTRACTIONS])
{
	storage_get_favourites(park_id, FAV_ATTRACTIONS, s_favs);
	s_fav_count = 0;
	for (int i = 0; i < MAX_FAVS_PER_LIST; i++) {
		if (s_favs[i][0]) s_fav_count++;
	}
	s_mode = mode;

	uint8_t n = g_park.attraction_count;
	for (uint8_t i = 0; i < n; i++) out[i] = i;
	insertion_sort_attr(out, n);
	return n;
}

/* Shows: filter out empty-times, then favourites first, rest in PKJS order. */
uint8_t sort_shows(const char *park_id, uint8_t out[MAX_SHOWS])
{
	storage_get_favourites(park_id, FAV_SHOWS, s_favs);

	uint8_t count = 0;
	/* Pass 1: favourites with upcoming times */
	for (uint8_t i = 0; i < g_park.show_count; i++) {
		const show_t *s = &g_park.shows[i];
		if (s->time_count == 0) continue;
		if (is_fav(s->id)) out[count++] = i;
	}
	/* Pass 2: non-favourites with upcoming times */
	for (uint8_t i = 0; i < g_park.show_count; i++) {
		const show_t *s = &g_park.shows[i];
		if (s->time_count == 0) continue;
		if (!is_fav(s->id)) out[count++] = i;
	}
	return count;
}
