/*
 * favs_sync.h — pure builder for the FAVS_SYNC wire payload
 * (watch → phone favourites sync).
 *
 * Wire format (parsed by lib.parseFavsSync in src/pkjs/lib.js — keep in
 * sync):
 *   "<parkUuid>|a:<id,id,...>|h:<id,id,...>"
 * where a = attractions, h = shows, ids comma-joined in list order.
 * Empty lists encode as an empty id run ("a:"), so unfavouriting the
 * last item still syncs the (now empty) truth. The payload is always
 * the FULL current state for one park — never a delta — so a dropped
 * or reordered message self-heals on the next toggle.
 *
 * Size: worst case is 36 (uuid) + 2*(3 + 8*11 + 7) + 1 NUL = 233 bytes,
 * inside api.c's 256-byte outbox with room for the dict/tuple overhead
 * (~8 bytes). Ids are the 8-hex-char short ids in practice, ~205 max.
 *
 * Pure (host-testable): no Pebble APIs; storage.h is included only for
 * the MAX_FAVS_PER_LIST / FAV_ID_LEN array shape (the test build
 * resolves its <pebble.h> include to the stub header).
 */
#pragma once

#include "storage.h"

/* Encode park_id + both favourite lists (empty slots have id[0]=='\0',
 * as produced by storage_get_favourites) into out. Returns the length
 * written (excluding NUL), or -1 — with out set to "" — when out can't
 * hold the complete payload or park_id is unusable (NULL, empty, or
 * containing the '|' separator). Ids containing '|' or ',' would break
 * the framing and are skipped (they can't occur: ids are hex short
 * ids, but the encoder must not trust that). */
int favs_sync_encode(const char *park_id,
                     const char attrs[MAX_FAVS_PER_LIST][FAV_ID_LEN],
                     const char shows[MAX_FAVS_PER_LIST][FAV_ID_LEN],
                     char *out, int outsz);
