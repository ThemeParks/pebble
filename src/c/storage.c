/*
 * storage.c — persist API wrappers.
 *
 * Persist key layout:
 *   1                  : sort_mode (uint32)
 *   2                  : last_park (string, park UUID)
 *   1000 + parkSlot*2 + listKind : favourites array (MAX_FAVS_PER_LIST * FAV_ID_LEN bytes)
 *
 * parkSlot is the park's index in g_parks (0..3). We avoid hashing the
 * UUID — a fixed slot per known park is plenty.
 */

#include "storage.h"
#include "config.h"
#include <string.h>

#define KEY_SORT_MODE 1
#define KEY_LAST_PARK 2
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

void storage_init(void) {}

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

/* Read favourites blob into a fixed array. Empty slots have id[0]=='\0'. */
int storage_get_favourites(const char *park_id, fav_list_kind_t list_kind,
                           char out[MAX_FAVS_PER_LIST][FAV_ID_LEN])
{
	memset(out, 0, MAX_FAVS_PER_LIST * FAV_ID_LEN);
	int slot = park_slot(park_id);
	if (slot < 0) return 0;
	int key = fav_key(slot, list_kind);
	if (!persist_exists(key)) return 0;
	int n = persist_read_data(key, out, MAX_FAVS_PER_LIST * FAV_ID_LEN);
	if (n < 0) return 0;
	int count = 0;
	for (int i = 0; i < MAX_FAVS_PER_LIST; i++) {
		if (out[i][0] != '\0') count++;
	}
	return count;
}

bool storage_is_favourite(const char *park_id, fav_list_kind_t list_kind, const char *id)
{
	if (!id || !id[0]) return false;
	char favs[MAX_FAVS_PER_LIST][FAV_ID_LEN];
	storage_get_favourites(park_id, list_kind, favs);
	for (int i = 0; i < MAX_FAVS_PER_LIST; i++) {
		if (favs[i][0] && strcmp(favs[i], id) == 0) return true;
	}
	return false;
}

/* Read "open|close" back from persist; split on '|' into the out buffers. */
bool storage_get_park_hours(const char *park_id,
                            char open_out[PARK_HOURS_STR_LEN],
                            char close_out[PARK_HOURS_STR_LEN])
{
	int slot = park_slot(park_id);
	if (slot < 0) return false;
	int key = KEY_HOURS_BASE + slot;
	if (!persist_exists(key)) return false;
	char buf[PARK_HOURS_STR_LEN * 2 + 2];
	int n = persist_read_string(key, buf, sizeof(buf));
	if (n <= 0) return false;
	char *sep = strchr(buf, '|');
	if (!sep) return false;
	*sep = '\0';
	strncpy(open_out, buf, PARK_HOURS_STR_LEN - 1);
	open_out[PARK_HOURS_STR_LEN - 1] = '\0';
	strncpy(close_out, sep + 1, PARK_HOURS_STR_LEN - 1);
	close_out[PARK_HOURS_STR_LEN - 1] = '\0';
	return true;
}

void storage_set_park_hours(const char *park_id,
                            const char *open, const char *close)
{
	int slot = park_slot(park_id);
	if (slot < 0) return;
	if (!open || !close) return;
	char buf[PARK_HOURS_STR_LEN * 2 + 2];
	snprintf(buf, sizeof(buf), "%s|%s", open, close);
	persist_write_string(KEY_HOURS_BASE + slot, buf);
}

void storage_toggle_favourite(const char *park_id, fav_list_kind_t list_kind, const char *id)
{
	if (!id || !id[0]) return;
	int slot = park_slot(park_id);
	if (slot < 0) return;

	char favs[MAX_FAVS_PER_LIST][FAV_ID_LEN];
	storage_get_favourites(park_id, list_kind, favs);

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
