/*
 * storage.h — persistent storage (favourites, sort mode, last park).
 *
 * Backed by Pebble's persist_* API. Favourites are stored per PARK
 * IDENTITY (up to 8 fixed-length ids per park per list kind): a small
 * allocation index maps park UUID → persist entry, allocated on first
 * toggle and LRU-evicted at MAX_FAV_PARKS parks. Identity keying means
 * favourites survive the uber app's destination switches (g_parks slots
 * are repopulated per destination; favourites deliberately do NOT key
 * off g_parks at all). See the layout comment in storage.c.
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
/* Parks with an allocated favourites entry. Full table → LRU eviction
 * (that park's favourites drop — fine at 12: it hasn't been opened in
 * ages, and re-favouriting is one long-press). */
#define MAX_FAV_PARKS 12
/* Entry field for the park UUID (36 chars) + slack; matches the 40-byte
 * park-id buffers used across the app. */
#define FAV_PARK_ID_LEN 40

void storage_init(void);

/* Call when g_parks is repopulated (uber destination switch): drops the
 * hours RAM cache and deletes the persisted per-slot hours, which would
 * otherwise surface the PREVIOUS destination's hours under the new
 * destination's park names (hours slots are positional; hours are cheap
 * to refetch). Favourites are identity-keyed and need no invalidation —
 * they survive the switch by design. */
void storage_on_parks_changed(void);

sort_mode_t storage_get_sort_mode(void);
void storage_set_sort_mode(sort_mode_t mode);

/* AppGlance on/off — set from the phone's config page (SETTINGS_DATA),
 * read by publish_glance() on exit. Defaults to true when never set. */
bool storage_get_glance_enabled(void);
void storage_set_glance_enabled(bool enabled);

/* Packed My Parks glance line ("MK 45m · EP 30m"), composed phone-side
 * and pushed with the settings payload. publish_glance() prefers it —
 * while expiry (epoch secs) is still in the future — over the locally
 * composed single-park wait rung. Get returns false when none stored
 * (out buffers untouched). Setting an empty/NULL text clears.
 * Keep in sync with SETTINGS_LINE_MAX in settings_parse.h. */
#define GLANCE_LINE_MAX 80
bool storage_get_glance_line(char out[GLANCE_LINE_MAX], uint32_t *expiry_out);
void storage_set_glance_line(const char *text, uint32_t expiry);

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

/* park_id is any UUID string (< FAV_PARK_ID_LEN); it does NOT have to
 * be in g_parks — that's what makes favourites destination-proof.
 * Reads never allocate an index entry; toggle allocates on demand. */
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
