/*
 * favs_sync.c — FAVS_SYNC payload builder. See favs_sync.h for the
 * format and size budget.
 */

#include "favs_sync.h"
#include <string.h>

/* Append src to out at *pos, bounds-checked. Returns false (leaving
 * *pos untouched) when it wouldn't fit including the NUL — the caller
 * treats any failed append as a whole-payload failure, because a
 * truncated sync would overwrite the phone's store with a partial
 * list. */
static bool append(char *out, int outsz, int *pos, const char *src)
{
	int n = (int)strlen(src);
	if (*pos + n + 1 > outsz) return false;
	memcpy(out + *pos, src, (size_t)n);
	*pos += n;
	out[*pos] = '\0';
	return true;
}

static bool id_is_safe(const char *id)
{
	for (const char *p = id; *p; p++) {
		if (*p == '|' || *p == ',') return false;
	}
	return true;
}

static bool append_list(char *out, int outsz, int *pos, const char *tag,
                        const char list[MAX_FAVS_PER_LIST][FAV_ID_LEN])
{
	if (!append(out, outsz, pos, tag)) return false;
	bool first = true;
	for (int i = 0; i < MAX_FAVS_PER_LIST; i++) {
		if (list[i][0] == '\0') continue;   /* empty slot */
		if (!id_is_safe(list[i])) continue; /* would break the framing */
		if (!first && !append(out, outsz, pos, ",")) return false;
		if (!append(out, outsz, pos, list[i])) return false;
		first = false;
	}
	return true;
}

int favs_sync_encode(const char *park_id,
                     const char attrs[MAX_FAVS_PER_LIST][FAV_ID_LEN],
                     const char shows[MAX_FAVS_PER_LIST][FAV_ID_LEN],
                     char *out, int outsz)
{
	if (!out || outsz <= 0) return -1;
	out[0] = '\0';
	if (!park_id || !park_id[0] || strchr(park_id, '|')) return -1;

	int pos = 0;
	if (append(out, outsz, &pos, park_id)
	    && append_list(out, outsz, &pos, "|a:", attrs)
	    && append_list(out, outsz, &pos, "|h:", shows)) {
		return pos;
	}
	out[0] = '\0';
	return -1;
}
