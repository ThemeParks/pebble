/*
 * sort.h — sort indices into g_park.attractions / g_park.shows.
 *
 * Pure index-based sorting — no copies of the data, just an index array
 * the UI iterates to render rows.
 */
#pragma once

#include "parkdata.h"
#include "storage.h"

/* sorted_attraction_indices: writes up to MAX_ATTRACTIONS indices into out.
 * Order: fav+operating, fav+closed, non-fav+operating, non-fav+closed.
 * Within each bucket sorted by `mode`. Returns count.
 */
uint8_t sort_attractions(const char *park_id, sort_mode_t mode,
                         uint8_t out[MAX_ATTRACTIONS]);

/* sort_shows: shows with no upcoming times are dropped. Favourites pinned
 * to top, rest in PKJS order. Returns count. */
uint8_t sort_shows(const char *park_id, uint8_t out[MAX_SHOWS]);

/* Find the entity with `id` in a sorted index array (as produced by
 * sort_attractions / sort_shows). `kind` says which table the indices
 * refer to. Returns the position within `sorted`, or -1 if absent.
 * Used to keep the cursor on the same ride/show across a background
 * refresh, where re-sorting can move every row. */
int sort_find_id(const uint8_t *sorted, uint8_t count,
                 fav_list_kind_t kind, const char *id);
