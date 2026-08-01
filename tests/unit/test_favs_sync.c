/*
 * test_favs_sync.c — host-side tests for the FAVS_SYNC payload builder.
 *
 * The JS side (tests/js/test_lib.js, parseFavsSync) round-trips against
 * the exact strings asserted here — if the format changes, both suites
 * must move together.
 */
#include "favs_sync.h"
#include "unit.h"
#include <string.h>
#include <stdio.h>

#define TEST_UUID "aaaaaaaa-1111-4abc-8def-000000000001"

typedef char fav_list_t[MAX_FAVS_PER_LIST][FAV_ID_LEN];

static void fill(fav_list_t list, int n, const char *prefix)
{
	memset(list, 0, sizeof(fav_list_t));
	for (int i = 0; i < n && i < MAX_FAVS_PER_LIST; i++) {
		snprintf(list[i], FAV_ID_LEN, "%s%d", prefix, i);
	}
}

static void test_empty_lists(void)
{
	fav_list_t a, h;
	fill(a, 0, "");
	fill(h, 0, "");
	char out[256];
	int n = favs_sync_encode(TEST_UUID, a, h, out, sizeof(out));
	ASSERT_EQ_STR(TEST_UUID "|a:|h:", out);
	ASSERT_EQ_INT((int)strlen(out), n);
}

static void test_typical_lists(void)
{
	fav_list_t a, h;
	fill(a, 2, "ride");
	fill(h, 1, "show");
	char out[256];
	int n = favs_sync_encode(TEST_UUID, a, h, out, sizeof(out));
	ASSERT_EQ_STR(TEST_UUID "|a:ride0,ride1|h:show0", out);
	ASSERT_EQ_INT((int)strlen(out), n);
}

static void test_holes_are_skipped(void)
{
	/* storage_get_favourites leaves holes where entries were toggled
	 * off — the encoder must skip them without emitting empty ids. */
	fav_list_t a, h;
	fill(a, 3, "r");
	a[1][0] = '\0';   /* punch a hole */
	fill(h, 0, "");
	char out[256];
	favs_sync_encode(TEST_UUID, a, h, out, sizeof(out));
	ASSERT_EQ_STR(TEST_UUID "|a:r0,r2|h:", out);
}

static void test_full_lists_fit_outbox_budget(void)
{
	/* Worst case: 36-char uuid + 2 full lists of maximum-length ids
	 * must stay inside the documented 233-byte budget (see favs_sync.h)
	 * — which itself must fit api.c's 256-byte outbox minus overhead. */
	fav_list_t a, h;
	memset(a, 0, sizeof(a));
	memset(h, 0, sizeof(h));
	for (int i = 0; i < MAX_FAVS_PER_LIST; i++) {
		snprintf(a[i], FAV_ID_LEN, "aaaaaaaaaa%d", i);   /* 11 chars */
		snprintf(h[i], FAV_ID_LEN, "hhhhhhhhhh%d", i);
	}
	char out[256];
	int n = favs_sync_encode(TEST_UUID, a, h, out, sizeof(out));
	ASSERT_TRUE(n > 0);
	ASSERT_TRUE(n <= 232);   /* 233 including the NUL */
}

static void test_truncation_is_all_or_nothing(void)
{
	/* A buffer too small for the complete payload must yield -1 and an
	 * empty string — a silently truncated list would overwrite the
	 * phone's store with partial truth. */
	fav_list_t a, h;
	fill(a, MAX_FAVS_PER_LIST, "ride");
	fill(h, MAX_FAVS_PER_LIST, "show");
	char out[48];
	int n = favs_sync_encode(TEST_UUID, a, h, out, sizeof(out));
	ASSERT_EQ_INT(-1, n);
	ASSERT_EQ_STR("", out);

	/* Zero/absurd sizes never write out of bounds. */
	ASSERT_EQ_INT(-1, favs_sync_encode(TEST_UUID, a, h, out, 0));
	ASSERT_EQ_INT(-1, favs_sync_encode(TEST_UUID, a, h, NULL, 256));
}

static void test_separator_safety(void)
{
	/* Ids are hex short ids in practice, but the encoder must not trust
	 * that: an id carrying a separator would break the framing, so it
	 * is skipped. A park id carrying '|' can't be framed at all. */
	fav_list_t a, h;
	fill(a, 2, "ok");
	snprintf(a[1], FAV_ID_LEN, "bad|id");
	fill(h, 1, "x");
	snprintf(h[0], FAV_ID_LEN, "bad,id");
	char out[256];
	favs_sync_encode(TEST_UUID, a, h, out, sizeof(out));
	ASSERT_EQ_STR(TEST_UUID "|a:ok0|h:", out);

	ASSERT_EQ_INT(-1, favs_sync_encode("bad|uuid", a, h, out, sizeof(out)));
	ASSERT_EQ_INT(-1, favs_sync_encode("", a, h, out, sizeof(out)));
	ASSERT_EQ_INT(-1, favs_sync_encode(NULL, a, h, out, sizeof(out)));
}

int main(void)
{
	RUN_TEST(test_empty_lists);
	RUN_TEST(test_typical_lists);
	RUN_TEST(test_holes_are_skipped);
	RUN_TEST(test_full_lists_fit_outbox_budget);
	RUN_TEST(test_separator_safety);
	RUN_TEST(test_truncation_is_all_or_nothing);
	return unit_summary();
}
