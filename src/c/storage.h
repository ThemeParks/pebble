/*
 * storage.h — persistent storage (favourites, sort mode, last park).
 *
 * Backed by Pebble's persist_* API. Favourites stored per park as a
 * bitmask indexed by ride/show position in the latest park data.
 *
 * Caveat: the bitmask is keyed by park UUID + a hash of (id, position).
 * Simpler approach: store up to 8 favourite IDs per park as fixed-len strings.
 */
#pragma once

#include <pebble.h>
#include <stdbool.h>

typedef enum {
	SORT_LONGEST = 0,
	SORT_SHORTEST = 1,
	SORT_AZ = 2,
} sort_mode_t;

#define MAX_FAVS_PER_LIST 8
#define FAV_ID_LEN 12

void storage_init(void);

/* Call when g_parks is repopulated (uber destination switch): drops the
 * favourites/hours RAM caches and deletes the persisted per-slot hours,
 * which would otherwise surface the PREVIOUS destination's hours under
 * the new destination's park names (slots are positional). Favourites
 * persist is kept — it's user data, re-read lazily per slot. */
void storage_on_parks_changed(void);

sort_mode_t storage_get_sort_mode(void);
void storage_set_sort_mode(sort_mode_t mode);

const char *storage_get_last_park(void);   /* returns NULL if unset */
void storage_set_last_park(const char *park_id);

#ifdef MULTI_DESTINATION
const char *storage_get_last_destination(void);  /* returns NULL if unset */
void storage_set_last_destination(const char *dest_id);
#endif

typedef enum {
	FAV_ATTRACTIONS = 0,
	FAV_SHOWS = 1,
} fav_list_kind_t;

bool storage_is_favourite(const char *park_id, fav_list_kind_t kind, const char *id);
void storage_toggle_favourite(const char *park_id, fav_list_kind_t kind, const char *id);
int storage_get_favourites(const char *park_id, fav_list_kind_t kind,
                           char out_ids[MAX_FAVS_PER_LIST][FAV_ID_LEN]);

/* Park hours cache — populated when the watch receives fresh data for a
 * park, read by the park picker to show "9 AM - 10 PM" under each name.
 * Returns false and leaves the out buffers untouched if we haven't
 * seen this park's schedule yet. */
#define PARK_HOURS_STR_LEN 12
bool storage_get_park_hours(const char *park_id,
                            char open_out[PARK_HOURS_STR_LEN],
                            char close_out[PARK_HOURS_STR_LEN]);
void storage_set_park_hours(const char *park_id,
                            const char *open, const char *close);
