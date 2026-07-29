# Known Issues

## Uber app: favourites are keyed by park *slot*, not park identity

`storage.c` keys favourites by the park's index in `g_parks`
(`KEY_FAV_BASE + slot*2 + kind`). In per-destination builds the mapping
slot→park is fixed, so this is fine. In the uber app, `g_parks` is
repopulated on every destination switch — slot 0 is "Magic Kingdom"
today and "Islands of Adventure" tomorrow.

Consequences:

- Favourite *lookups* are effectively safe: entries are matched by the
  ride's short id, and ids from another destination's park won't match,
  so wrong stars don't appear.
- But all destinations **share the same 8-entry budget per slot**:
  favourites toggled at destination A and destination B accumulate in
  the same persisted list, and when it fills, `storage_toggle_favourite`
  evicts the last entry — potentially deleting a favourite belonging to
  the other destination.

The park-hours variant of this bug (previous destination's hours shown
under the new destination's park names) was fixed by clearing hours on
destination switch (`storage_on_parks_changed()`), because hours are
cheap to refetch. Favourites are user data, so they must be re-keyed,
not wiped.

**Proposed fix:** key favourites by a hash of the destination id + slot
(or migrate to a small id-keyed table). Needs a one-time migration for
existing per-destination users (same persist layout, so their keys stay
valid — only the uber app needs the new scheme). Worth doing together
with the favourites→PKJS sync required by timeline pins and ride alerts
(see docs/future/).
