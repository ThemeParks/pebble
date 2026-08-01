/*
 * storage.c — persist API wrappers.
 *
 * Persist key layout:
 *   1                  : sort_mode (uint32)
 *   2                  : last_park (string, park UUID)
 *   3                  : glance_enabled (int 0/1; absent = enabled)
 *   4                  : glance_line (string, packed My Parks line)
 *   5                  : glance_line_expiry (uint32 epoch secs)
 *   6                  : fav_migrated (int 1 once the slot→identity
 *                        favourites migration has run; see below)
 *   200                : last_dest (string, uber build only)
 *   1000 + parkSlot*2 + listKind : LEGACY favourites array — the
 *                        pre-identity slot-keyed scheme. Read once by
 *                        the migration, then deleted. Never written.
 *   2000 + parkSlot    : park hours ("open|close" string)
 *   2900 + favEntry    : favourites allocation index entry
 *                        (fav_entry_t blob: park UUID + LRU tick).
 *                        One key per entry — the whole 12-entry table
 *                        would exceed PERSIST_DATA_MAX_LENGTH (256).
 *   3000 + favEntry*2 + listKind : favourites array
 *                        (MAX_FAVS_PER_LIST * FAV_ID_LEN bytes)
 *
 * parkSlot is the park's index in g_parks. Slot keying is fine for
 * HOURS (cheap to refetch, wiped on destination switch) but was a bug
 * for favourites in the uber app: g_parks is repopulated per
 * destination, so slot N meant a different park after every switch and
 * destinations shared (and evicted) each other's 8-entry budgets.
 * Favourites are therefore keyed by park IDENTITY: a small allocation
 * index maps park UUID → favEntry, allocated on first toggle,
 * regardless of g_parks membership — which is what makes the keys
 * destination-proof.
 */

#include "storage.h"
#include "config.h"
#include <string.h>

#define KEY_SORT_MODE 1
#define KEY_LAST_PARK 2
#define KEY_GLANCE_ENABLED 3
#define KEY_GLANCE_LINE 4
#define KEY_GLANCE_LINE_EXPIRY 5
#define KEY_FAV_MIGRATED 6
#define KEY_FAV_LEGACY_BASE 1000
/* Hours stored as "open|close" in one persist slot per park. */
#define KEY_HOURS_BASE 2000
#define KEY_FAV_INDEX_BASE 2900
#define KEY_FAV_BASE 3000
/* Widest park table the legacy scheme could have written slots for
 * (uber MAX_PARKS) — bounds the migration's delete sweep. */
#define LEGACY_FAV_MAX_SLOTS 10
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

/* --- favourites allocation index -------------------------------------
 * Maps park UUID → a persistent entry number (0..MAX_FAV_PARKS-1) that
 * owns the two favourites blobs at KEY_FAV_BASE + entry*2 + kind.
 * Entries are allocated on first toggle; when the table is full the
 * least-recently-used entry is evicted, dropping that park's favourites
 * — acceptable at 12 parks (the user hasn't looked at it in a long
 * time, and re-favouriting is one long-press). lru_tick is bumped when
 * a park's list is (re)loaded into the RAM cache or toggled, i.e. once
 * per park visit/mutation — never on the per-frame read paths. */
typedef struct {
	char park_id[FAV_PARK_ID_LEN];   /* "" = free entry */
	uint32_t lru_tick;               /* higher = more recently used */
} fav_entry_t;

static fav_entry_t s_fav_index[MAX_FAV_PARKS];
static bool s_fav_index_loaded = false;
static bool s_fav_migration_done = false;   /* RAM mirror of KEY_FAV_MIGRATED */
static uint32_t s_fav_next_tick = 1;        /* next tick to hand out */

/* Per-kind favourites RAM cache, keyed by allocated ENTRY (identity),
 * not g_parks slot — so it stays valid across uber destination
 * switches. */
static int s_fav_cache_entry[FAV_KINDS] = {-1, -1};
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

static int fav_key(int entry, fav_list_kind_t list_kind)
{
	return KEY_FAV_BASE + entry * 2 + list_kind;
}

static void fav_index_persist_entry(int n)
{
	persist_write_data(KEY_FAV_INDEX_BASE + n, &s_fav_index[n],
	                   sizeof(fav_entry_t));
}

/* Linear scan for a park's entry. -1 when unallocated. 12 strcmps max —
 * cheap enough for the draw path (all RAM). */
static int fav_entry_find(const char *park_id)
{
	for (int n = 0; n < MAX_FAV_PARKS; n++) {
		if (s_fav_index[n].park_id[0]
		    && strcmp(s_fav_index[n].park_id, park_id) == 0) return n;
	}
	return -1;
}

/* Mark an entry most-recently-used. No-op (no flash write) when it
 * already is — so repeated touches from one park's session cost one
 * persist write, not one per call. */
static void fav_entry_touch(int n)
{
	if (s_fav_index[n].lru_tick + 1 == s_fav_next_tick) return;
	s_fav_index[n].lru_tick = s_fav_next_tick++;
	fav_index_persist_entry(n);
}

/* Find-or-allocate an entry for park_id. Prefers a free entry; a full
 * table evicts the LRU entry, deleting its favourites blobs. */
static int fav_entry_alloc(const char *park_id)
{
	int n = fav_entry_find(park_id);
	if (n >= 0) {
		fav_entry_touch(n);
		return n;
	}
	int victim = -1;
	for (int i = 0; i < MAX_FAV_PARKS; i++) {
		if (s_fav_index[i].park_id[0] == '\0') { victim = i; break; }
	}
	if (victim < 0) {
		/* Table full — evict the least-recently-used park. Its
		 * favourites are dropped (documented trade-off: with 12 entries
		 * this only hits a park the user hasn't opened in ages). */
		uint32_t best = s_fav_index[0].lru_tick;
		victim = 0;
		for (int i = 1; i < MAX_FAV_PARKS; i++) {
			if (s_fav_index[i].lru_tick < best) {
				best = s_fav_index[i].lru_tick;
				victim = i;
			}
		}
		for (int k = 0; k < FAV_KINDS; k++) {
			persist_delete(fav_key(victim, (fav_list_kind_t)k));
			/* The RAM cache may still hold the evicted park's list under
			 * this entry number — invalidate so the new owner reloads. */
			if (s_fav_cache_entry[k] == victim) s_fav_cache_entry[k] = -1;
		}
	}
	strncpy(s_fav_index[victim].park_id, park_id, FAV_PARK_ID_LEN - 1);
	s_fav_index[victim].park_id[FAV_PARK_ID_LEN - 1] = '\0';
	/* Assign the tick directly (not via touch — its already-MRU check
	 * misfires on a fresh entry whose tick happens to be next-1). */
	s_fav_index[victim].lru_tick = s_fav_next_tick++;
	fav_index_persist_entry(victim);
	return victim;
}

/* One-time migration from the legacy slot-keyed favourites (keys
 * 1000 + slot*2 + kind) to identity keys, using the CURRENT g_parks
 * mapping. Per-destination builds: exact (the slot→park mapping never
 * changed). Uber: honest best effort — the blobs are re-keyed as the
 * current/last destination's parks, which is where they were last
 * meaningful; slots beyond the current park count (another
 * destination's leftovers, the very pollution this fix removes) are
 * unmappable and deleted.
 *
 * Trigger: lazily, from the first favourites access of a launch
 * (fav_index_ensure), NOT storage_init — in the uber build g_parks is
 * still empty at init (parks arrive from the phone), so init-time
 * migration would find no mapping. "Done" is an EXPLICIT marker key
 * (KEY_FAV_MIGRATED), not the index's existence: the index is also
 * created by a first toggle, which must not be able to strand old
 * blobs unmigrated. While g_parks is empty the marker stays unset and
 * the next access retries. */
static void fav_migrate_legacy(void)
{
	if (persist_exists(KEY_FAV_MIGRATED)) {
		s_fav_migration_done = true;
		return;
	}
	if (NUM_PARKS == 0) return;   /* uber, parks not in yet — retry later */

	for (int slot = 0; slot < (int)NUM_PARKS && slot < LEGACY_FAV_MAX_SLOTS;
	     slot++) {
		for (int k = 0; k < FAV_KINDS; k++) {
			int old_key = KEY_FAV_LEGACY_BASE + slot * 2 + k;
			if (!persist_exists(old_key)) continue;
			char blob[MAX_FAVS_PER_LIST][FAV_ID_LEN];
			memset(blob, 0, sizeof(blob));
			persist_read_data(old_key, blob, sizeof(blob));
			persist_delete(old_key);
			bool any = false;
			for (int i = 0; i < MAX_FAVS_PER_LIST; i++) {
				if (blob[i][0]) { any = true; break; }
			}
			if (!any) continue;   /* empty list — nothing worth an entry */
			int n = fav_entry_alloc(g_parks[slot].id);
			if (n < 0) continue;
			persist_write_data(fav_key(n, (fav_list_kind_t)k), blob,
			                   sizeof(blob));
		}
	}
	/* Sweep any unmappable leftovers (see comment above). */
	for (int slot = 0; slot < LEGACY_FAV_MAX_SLOTS; slot++) {
		for (int k = 0; k < FAV_KINDS; k++) {
			persist_delete(KEY_FAV_LEGACY_BASE + slot * 2 + k);
		}
	}
	persist_write_int(KEY_FAV_MIGRATED, 1);
	s_fav_migration_done = true;
}

/* Lazy-load the allocation index (once per launch), then give the
 * legacy migration its chance. Every public favourites call funnels
 * through here; after the first call it's pure RAM checks. */
static void fav_index_ensure(void)
{
	if (!s_fav_index_loaded) {
		memset(s_fav_index, 0, sizeof(s_fav_index));
		uint32_t max_tick = 0;
		for (int n = 0; n < MAX_FAV_PARKS; n++) {
			int key = KEY_FAV_INDEX_BASE + n;
			if (!persist_exists(key)) continue;
			persist_read_data(key, &s_fav_index[n], sizeof(fav_entry_t));
			/* A short/garbled blob must not yield an unterminated id. */
			s_fav_index[n].park_id[FAV_PARK_ID_LEN - 1] = '\0';
			if (s_fav_index[n].park_id[0]
			    && s_fav_index[n].lru_tick > max_tick) {
				max_tick = s_fav_index[n].lru_tick;
			}
		}
		s_fav_next_tick = max_tick + 1;
		s_fav_index_loaded = true;
	}
	if (!s_fav_migration_done) fav_migrate_legacy();
}

/* Load one favourites list into the RAM cache if it isn't already the
 * cached entry. Kind and entry pair up: switching parks reloads. */
static void fav_cache_load(int entry, fav_list_kind_t kind)
{
	if (s_fav_cache_entry[kind] == entry) return;
	memset(s_fav_cache[kind], 0, sizeof(s_fav_cache[kind]));
	int key = fav_key(entry, kind);
	if (persist_exists(key)) {
		persist_read_data(key, s_fav_cache[kind], sizeof(s_fav_cache[kind]));
	}
	s_fav_cache_entry[kind] = entry;
	/* A (re)load means the user is looking at this park — LRU-worthy. */
	fav_entry_touch(entry);
}

void storage_on_parks_changed(void)
{
	/* The uber app reuses HOURS persist slots across destinations (slot
	 * N is "the Nth park of the current destination"), so on a
	 * destination switch the cached hours belong to the WRONG parks —
	 * drop both the RAM cache and the persisted hours. Favourites need
	 * NOTHING here any more: they're keyed by park identity (allocation
	 * index above), so both their persist keys and their entry-keyed RAM
	 * caches stay correct across the switch — that's the point of the
	 * re-key. */
	for (int i = 0; i < HOURS_CACHE_SLOTS; i++) {
		s_hours_state[i] = HOURS_UNKNOWN;
		persist_delete(KEY_HOURS_BASE + i);
	}
}

/* g_parks slot lookup — still the right key for HOURS (positional data,
 * wiped on destination switch). Favourites no longer use it. */
static int park_slot(const char *park_id)
{
	if (!park_id) return -1;
	for (int i = 0; i < NUM_PARKS; i++) {
		if (strcmp(g_parks[i].id, park_id) == 0) return i;
	}
	return -1;
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
	s_fav_index_loaded = false;
	s_fav_migration_done = false;
	s_fav_next_tick = 1;
	for (int k = 0; k < FAV_KINDS; k++) s_fav_cache_entry[k] = -1;
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
 * have id[0]=='\0'. Read paths never ALLOCATE an index entry — a park
 * with no favourites yet simply reads empty. */
int storage_get_favourites(const char *park_id, fav_list_kind_t list_kind,
                           char out[MAX_FAVS_PER_LIST][FAV_ID_LEN])
{
	memset(out, 0, MAX_FAVS_PER_LIST * FAV_ID_LEN);
	if (!park_id || !park_id[0]) return 0;
	fav_index_ensure();
	int entry = fav_entry_find(park_id);
	if (entry < 0) return 0;
	fav_cache_load(entry, list_kind);
	memcpy(out, s_fav_cache[list_kind], MAX_FAVS_PER_LIST * FAV_ID_LEN);
	int count = 0;
	for (int i = 0; i < MAX_FAVS_PER_LIST; i++) {
		if (out[i][0] != '\0') count++;
	}
	return count;
}

bool storage_is_favourite(const char *park_id, fav_list_kind_t list_kind, const char *id)
{
	if (!id || !id[0] || !park_id || !park_id[0]) return false;
	/* Called per row per frame by the park view's draw path — must stay
	 * a pure RAM scan after the first load. */
	fav_index_ensure();
	int entry = fav_entry_find(park_id);
	if (entry < 0) return false;
	fav_cache_load(entry, list_kind);
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
	if (!id || !id[0] || !park_id || !park_id[0]) return;
	/* An id longer than the entry field would be truncated on store and
	 * then collide with every park sharing the prefix — refuse instead.
	 * Real park UUIDs are 36 chars, comfortably inside. */
	if (strlen(park_id) >= FAV_PARK_ID_LEN) return;
	fav_index_ensure();
	int entry = fav_entry_alloc(park_id);
	if (entry < 0) return;

	/* Mutate the RAM cache in place, then write it through to persist —
	 * keeps cache and flash in lockstep with one write. */
	fav_cache_load(entry, list_kind);
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

	persist_write_data(fav_key(entry, list_kind), favs,
	                   MAX_FAVS_PER_LIST * FAV_ID_LEN);
}
