# Multi-Destination App — Design Notes

## Concept

An "uber app" that lets users switch between destinations at runtime (WDW, Universal Orlando, Disneyland Paris, etc.), while also keeping the per-destination apps on the store. Same codebase, two build modes:

- **Per-destination builds** (existing): hardcoded parks, custom icon, easy to find on store as "WDW Queues"
- **Multi-destination build**: fetches destination list from API, user picks destination → picks park

## Open question: park icons

Each destination has custom park icons (castle, sphere, star, tree for WDW; roller coaster, globe, etc. for Universal). Currently these are compiled-in as C drawing functions selected by a string token in `config.h`.

Options:
- **Ship all icons in the uber app**: every destination's icons baked in. Binary size grows but Pebble has 256KB for resources. Straightforward — just a bigger icon token table.
- **Generic icons by park type**: map park entity types (THEME_PARK, WATER_PARK) to generic icons. Loses personality but no scaling problem.
- **Download icon metadata from API**: API returns an icon token per park, watch maps to its built-in set. New destinations Just Work if they reuse existing tokens; new icon shapes need an app update.
- **Hybrid**: ship a large built-in set covering all known destinations, fall back to a circle for unknowns. API returns the token so the mapping is server-driven.

The hybrid approach is probably right. The icon drawing functions are tiny (~20 lines of GPath each). Shipping 20-30 of them costs almost nothing. The API already has the data to drive the mapping.

## Architecture sketch

- Destination list: fetched from `/v1/destinations` on first launch, cached in persist storage
- Destination picker: new screen above the park picker (or a section in settings)
- Park list: fetched from `/v1/entity/{destId}/children` or embedded in the destination response
- Build system: `build_dest.py all` produces the uber app with all icons, no hardcoded parks
- Per-destination builds: unchanged, keep working as today
- Migration: existing per-destination app users who switch to the uber app lose favourites (different app UUID = different persist storage). Could offer an import flow but probably not worth the complexity — favourites are quick to re-set.

## Scope

Multi-day project. Touches build system, C data model, PKJS, persist storage, appstore listing. Should be its own brainstorming → spec → plan cycle.
