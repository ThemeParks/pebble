/*
 * storage.c — persist API wrappers.
 *
 * Persist key layout:
 *   1                  : sort_mode (uint32)
 *   2                  : last_park (string, park UUID)
 *   3                  : glance_enabled (int 0/1; absent = enabled)
 *   4                  : glance_line (string, packed My Parks line)
 *   5                  : glance_line_expiry (uint32 epoch secs)
 *   200                : last_dest (string, uber build only)
 *   1000 + parkSlot*2 + listKind : favourites array (MAX_FAVS_PER_LIST * FAV_ID_LEN bytes)
 *   2000 + parkSlot    : park hours ("open|close" string)
 *
 * parkSlot is the park's index in g_parks (0..3). We avoid hashing the
 * UUID — a fixed slot per known park is plenty.
 */

#include "storage.h"
#include "config.h"
#include <string.h>

#define KEY_SORT_MODE 1
#define KEY_LAST_PARK 2
#define KEY_GLANCE_ENABLED 3
#define KEY_GLANCE_LINE 4
#define KEY_GLANCE_LINE_EXPIRY 5
#define KEY_FAV_BASE 1000
/* Hours stored as "open|close" in one persist slot per park. */
#define KEY_HOURS_BASE 2000
#ifdef MULTI_DESTINATION
#define KEY_LAST_DEST 200
#endif

static char s_last_park_buf[40];
#ifdef MULTI_DESTINATION
static char s_last_dest_buf[40];
#endif

/* --- RAM caches ------------------------------------------------------
 * MenuLayer draw callbacks ask for favourites and park hours per visible
 * row on every animation frame; persist_* reads go to flash. Both are
 * cached in RAM with write-through on mutation, so the draw paths never
 * touch flash after the first query. */

#define FAV_KINDS 2
static int s_fav_cache_slot[FAV_KINDS] = {-1, -1};
static char s_fav_cache[FAV_KINDS][MAX_FAVS_PER_LIST][FAV_ID_LEN];

/* Sized for the largest park table any build ships (uber MAX_PARKS 10). */
#define HOURS_CACHE_SLOTS 10
typedef enum {
	HOURS_UNKNOWN = 0,   /* not asked persist yet */
	HOURS_MISSING,       /* asked; nothing stored */
	HOURS_CACHED,        /* open/close strings below are valid */
} hours_state_t;
static uint8_t s_hours_state[HOURS_CACHE_SLOTS];
static char s_hours_open[HOURS_CACHE_SLOTS][PARK_HOURS_STR_LEN];
static char s_hours_close[HOURS_CACHE_SLOTS][PARK_HOURS_STR_LEN];

/* Load one favourites list into the RAM cache if it isn't already the
 * cached slot. Kind and slot pair up: switching parks reloads. */
static void fav_cache_load(int slot, fav_list_kind_t kind)
{
	if (s_fav_cache_slot[kind] == slot) return;
	memset(s_fav_cache[kind], 0, sizeof(s_fav_cache[kind]));
	int key = KEY_FAV_BASE + slot * 2 + kind;
	if (persist_exists(key)) {
		persist_read_data(key, s_fav_cache[kind], sizeof(s_fav_cache[kind]));
	}
	s_fav_cache_slot[kind] = slot;
}

void storage_on_parks_changed(void)
{
	/* The uber app reuses persist slots across destinations (slot N is
	 * "the Nth park of the current destination"), so on a destination
	 * switch the cached hours belong to the WRONG parks — drop both the
	 * RAM caches and the persisted hours. Favourites persist stays (it's
	 * user data and repopulates per-slot); only its RAM cache drops. */
	for (int k = 0; k < FAV_KINDS; k++) s_fav_cache_slot[k] = -1;
	for (int i = 0; i < HOURS_CACHE_SLOTS; i++) {
		s_hours_state[i] = HOURS_UNKNOWN;
		persist_delete(KEY_HOURS_BASE + i);
	}
}

static int park_slot(const char *park_id)
{
	if (!park_id) return -1;
	for (int i = 0; i < NUM_PARKS; i++) {
		if (strcmp(g_parks[i].id, park_id) == 0) return i;
	}
	return -1;
}

static int fav_key(int slot, fav_list_kind_t list_kind)
{
	return KEY_FAV_BASE + slot * 2 + list_kind;
}

/* Glance flag cache: -1 = not read yet, else 0/1. It's only read on the
 * deinit path (publish_glance), so a simple lazy read suffices — the
 * cache mostly spares repeat reads within one exit. */
static int8_t s_glance_cache = -1;

/* Glance line cache — same tri-state pattern as the hours cache:
 * "missing" is cached too, so an unset line costs one persist probe. */
typedef enum {
	LINE_UNKNOWN = 0,   /* not asked persist yet */
	LINE_MISSING,       /* asked; nothing stored */
	LINE_CACHED,        /* buffers below are valid */
} line_state_t;
static uint8_t s_line_state = LINE_UNKNOWN;
static char s_line_buf[GLANCE_LINE_MAX];
static uint32_t s_line_expiry;

void storage_init(void)
{
	/* Statics are zeroed on every real launch; the unit-test binaries
	 * run many cases in one process, so reset_all() calls this to re-arm
	 * the lazy caches as if freshly launched. */
	s_glance_cache = -1;
	s_line_state = LINE_UNKNOWN;
}

bool storage_get_glance_enabled(void)
{
	if (s_glance_cache < 0) {
		/* Default enabled when never set — the glance predates this
		 * toggle, so upgrading users keep the behaviour they had. */
		s_glance_cache = 1;
		if (persist_exists(KEY_GLANCE_ENABLED)) {
			s_glance_cache = persist_read_int(KEY_GLANCE_ENABLED) ? 1 : 0;
		}
	}
	return s_glance_cache == 1;
}

void storage_set_glance_enabled(bool enabled)
{
	persist_write_int(KEY_GLANCE_ENABLED, enabled ? 1 : 0);
	s_glance_cache = enabled ? 1 : 0;
}

bool storage_get_glance_line(char out[GLANCE_LINE_MAX], uint32_t *expiry_out)
{
	if (s_line_state == LINE_UNKNOWN) {
		s_line_state = LINE_MISSING;
		if (persist_exists(KEY_GLANCE_LINE)
		    && persist_read_string(KEY_GLANCE_LINE, s_line_buf,
		                           sizeof(s_line_buf)) > 0
		    && s_line_buf[0] != '\0') {
			/* Expiry rides in its own slot; a missing one reads as 0 —
			 * i.e. already expired, which publish_glance ignores. Safer
			 * than inventing a future expiry for half-written state. */
			s_line_expiry = persist_exists(KEY_GLANCE_LINE_EXPIRY)
				? (uint32_t)persist_read_int(KEY_GLANCE_LINE_EXPIRY) : 0;
			s_line_state = LINE_CACHED;
		}
	}
	if (s_line_state != LINE_CACHED) return false;
	strncpy(out, s_line_buf, GLANCE_LINE_MAX - 1);
	out[GLANCE_LINE_MAX - 1] = '\0';
	if (expiry_out) *expiry_out = s_line_expiry;
	return true;
}

void storage_set_glance_line(const char *text, uint32_t expiry)
{
	if (!text || !text[0]) {
		/* Empty set is the explicit clear (user deselected all parks,
		 * or PKJS had nothing usable) — drop persist AND cache so the
		 * local single-park glance takes over immediately. */
		persist_delete(KEY_GLANCE_LINE);
		persist_delete(KEY_GLANCE_LINE_EXPIRY);
		s_line_state = LINE_MISSING;
		return;
	}
	persist_write_string(KEY_GLANCE_LINE, text);
	persist_write_int(KEY_GLANCE_LINE_EXPIRY, (int)expiry);
	/* Write through to the RAM cache. */
	strncpy(s_line_buf, text, sizeof(s_line_buf) - 1);
	s_line_buf[sizeof(s_line_buf) - 1] = '\0';
	s_line_expiry = expiry;
	s_line_state = LINE_CACHED;
}

sort_mode_t storage_get_sort_mode(void)
{
	if (!persist_exists(KEY_SORT_MODE)) return SORT_LONGEST;
	int v = persist_read_int(KEY_SORT_MODE);
	if (v < 0 || v > SORT_AZ) return SORT_LONGEST;
	return (sort_mode_t)v;
}

void storage_set_sort_mode(sort_mode_t mode)
{
	persist_write_int(KEY_SORT_MODE, (int)mode);
}

const char *storage_get_last_park(void)
{
	if (!persist_exists(KEY_LAST_PARK)) return NULL;
	int n = persist_read_string(KEY_LAST_PARK, s_last_park_buf, sizeof(s_last_park_buf));
	if (n <= 0) return NULL;
	return s_last_park_buf;
}

void storage_set_last_park(const char *park_id)
{
	if (!park_id) return;
	persist_write_string(KEY_LAST_PARK, park_id);
}

#ifdef MULTI_DESTINATION
const char *storage_get_last_destination(void)
{
	if (!persist_exists(KEY_LAST_DEST)) return NULL;
	int n = persist_read_string(KEY_LAST_DEST, s_last_dest_buf, sizeof(s_last_dest_buf));
	if (n <= 0) return NULL;
	return s_last_dest_buf;
}

void storage_set_last_destination(const char *dest_id)
{
	if (!dest_id) return;
	persist_write_string(KEY_LAST_DEST, dest_id);
}
#endif

/* Read favourites into a fixed array (from the RAM cache — flash is
 * only touched the first time a park's list is asked for). Empty slots
 * have id[0]=='\0'. */
int storage_get_favourites(const char *park_id, fav_list_kind_t list_kind,
                           char out[MAX_FAVS_PER_LIST][FAV_ID_LEN])
{
	memset(out, 0, MAX_FAVS_PER_LIST * FAV_ID_LEN);
	int slot = park_slot(park_id);
	if (slot < 0) return 0;
	fav_cache_load(slot, list_kind);
	memcpy(out, s_fav_cache[list_kind], MAX_FAVS_PER_LIST * FAV_ID_LEN);
	int count = 0;
	for (int i = 0; i < MAX_FAVS_PER_LIST; i++) {
		if (out[i][0] != '\0') count++;
	}
	return count;
}

bool storage_is_favourite(const char *park_id, fav_list_kind_t list_kind, const char *id)
{
	if (!id || !id[0]) return false;
	int slot = park_slot(park_id);
	if (slot < 0) return false;
	/* Called per row per frame by the park view's draw path — must stay
	 * a pure RAM scan after the first load. */
	fav_cache_load(slot, list_kind);
	for (int i = 0; i < MAX_FAVS_PER_LIST; i++) {
		if (s_fav_cache[list_kind][i][0]
		    && strcmp(s_fav_cache[list_kind][i], id) == 0) return true;
	}
	return false;
}

/* Read "open|close" hours — from the RAM cache after the first query;
 * persist is consulted once per slot ("missing" is cached too, so parks
 * without stored hours don't re-probe flash every frame). */
bool storage_get_park_hours(const char *park_id,
                            char open_out[PARK_HOURS_STR_LEN],
                            char close_out[PARK_HOURS_STR_LEN])
{
	int slot = park_slot(park_id);
	if (slot < 0 || slot >= HOURS_CACHE_SLOTS) return false;

	if (s_hours_state[slot] == HOURS_UNKNOWN) {
		s_hours_state[slot] = HOURS_MISSING;
		int key = KEY_HOURS_BASE + slot;
		char buf[PARK_HOURS_STR_LEN * 2 + 2];
		if (persist_exists(key)
		    && persist_read_string(key, buf, sizeof(buf)) > 0) {
			char *sep = strchr(buf, '|');
			if (sep) {
				*sep = '\0';
				strncpy(s_hours_open[slot], buf, PARK_HOURS_STR_LEN - 1);
				s_hours_open[slot][PARK_HOURS_STR_LEN - 1] = '\0';
				strncpy(s_hours_close[slot], sep + 1, PARK_HOURS_STR_LEN - 1);
				s_hours_close[slot][PARK_HOURS_STR_LEN - 1] = '\0';
				s_hours_state[slot] = HOURS_CACHED;
			}
		}
	}

	if (s_hours_state[slot] != HOURS_CACHED) return false;
	strncpy(open_out, s_hours_open[slot], PARK_HOURS_STR_LEN - 1);
	open_out[PARK_HOURS_STR_LEN - 1] = '\0';
	strncpy(close_out, s_hours_close[slot], PARK_HOURS_STR_LEN - 1);
	close_out[PARK_HOURS_STR_LEN - 1] = '\0';
	return true;
}

void storage_set_park_hours(const char *park_id,
                            const char *open, const char *close)
{
	int slot = park_slot(park_id);
	if (slot < 0 || slot >= HOURS_CACHE_SLOTS) return;
	if (!open || !close) return;
	char buf[PARK_HOURS_STR_LEN * 2 + 2];
	snprintf(buf, sizeof(buf), "%s|%s", open, close);
	persist_write_string(KEY_HOURS_BASE + slot, buf);
	/* Write through to the RAM cache. */
	strncpy(s_hours_open[slot], open, PARK_HOURS_STR_LEN - 1);
	s_hours_open[slot][PARK_HOURS_STR_LEN - 1] = '\0';
	strncpy(s_hours_close[slot], close, PARK_HOURS_STR_LEN - 1);
	s_hours_close[slot][PARK_HOURS_STR_LEN - 1] = '\0';
	s_hours_state[slot] = HOURS_CACHED;
}

void storage_toggle_favourite(const char *park_id, fav_list_kind_t list_kind, const char *id)
{
	if (!id || !id[0]) return;
	int slot = park_slot(park_id);
	if (slot < 0) return;

	/* Mutate the RAM cache in place, then write it through to persist —
	 * keeps cache and flash in lockstep with one write. */
	fav_cache_load(slot, list_kind);
	char (*favs)[FAV_ID_LEN] = s_fav_cache[list_kind];

	/* Toggle: remove if present, otherwise add to first empty slot. */
	int found = -1;
	int empty = -1;
	for (int i = 0; i < MAX_FAVS_PER_LIST; i++) {
		if (favs[i][0] == '\0') {
			if (empty < 0) empty = i;
		} else if (strcmp(favs[i], id) == 0) {
			found = i;
			break;
		}
	}

	if (found >= 0) {
		favs[found][0] = '\0';
	} else if (empty >= 0) {
		strncpy(favs[empty], id, FAV_ID_LEN - 1);
		favs[empty][FAV_ID_LEN - 1] = '\0';
	} else {
		/* All slots full — replace the last one */
		strncpy(favs[MAX_FAVS_PER_LIST - 1], id, FAV_ID_LEN - 1);
		favs[MAX_FAVS_PER_LIST - 1][FAV_ID_LEN - 1] = '\0';
	}

	persist_write_data(fav_key(slot, list_kind), favs,
	                   MAX_FAVS_PER_LIST * FAV_ID_LEN);
}
