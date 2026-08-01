# Known Issues

## FIXED — Uber app: favourites were keyed by park *slot*, not identity

> Fixed on `feat/fav-rekey`. Original issue kept below for history;
> the fix and migration are described at the end.

`storage.c` used to key favourites by the park's index in `g_parks`
(`1000 + slot*2 + kind`). In per-destination builds the mapping
slot→park is fixed, so this was fine. In the uber app, `g_parks` is
repopulated on every destination switch — slot 0 is "Magic Kingdom"
today and "Islands of Adventure" tomorrow — so all destinations shared
the same 8-entry budget per slot, and a full list evicted favourites
belonging to another destination.

### The fix (identity keying)

Favourites are now keyed by the **park UUID** through a small
persisted allocation index (`2900 + entry`: park UUID + LRU tick, 12
entries; favourites blobs at `3000 + entry*2 + kind`). Every park —
any destination, even parks not in the current `g_parks` — gets its
own 8-per-list budget. When all 12 entries are in use, the
least-recently-used park's entry is evicted (dropping only that
park's favourites — acceptable at 12 parks tracked).

`storage_on_parks_changed()` (uber destination switch) no longer
touches favourites at all — identity keys survive switches by
construction. Hours stay slot-keyed and wiped on switch (cheap to
refetch, positional by design).

### Migration

One-time, **lazy** — it runs at the first favourites access of a
launch, not `storage_init()`, because the uber build's `g_parks` is
empty until the phone sends parks. Completion is an explicit marker
key (`6`), not the new index's existence, so a first toggle can't
strand old blobs unmigrated; while `g_parks` is empty the marker
stays unset and the next access retries. Per-destination builds
migrate exactly (their slot→park mapping never changed); the uber
build re-keys old blobs as the current/last destination's parks —
honest best effort, since that's where they were last meaningful —
and deletes unmappable leftovers (the cross-destination pollution
this fix removes). Legacy keys are deleted after migration.
