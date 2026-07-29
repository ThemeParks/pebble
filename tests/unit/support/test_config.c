/*
 * test_config.c — fake park table for host-side tests.
 *
 * Compiled against the checked-in src/c/config.h (NUM_PARKS == 4),
 * replacing src/c/config.c so tests don't depend on real WDW UUIDs.
 * IDs just need to be distinct — storage.c maps them to persist slots
 * by table index, so tests exercise the same slot math as production.
 */
#include "config.h"

const park_config_t g_parks[NUM_PARKS] = {
	{ "test-park-alpha", "Alpha Park" },
	{ "test-park-beta", "Beta Gardens" },
	{ "test-park-gamma", "Gamma World" },
	{ "test-park-delta", "Delta Land" },
};
