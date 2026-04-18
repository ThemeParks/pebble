/*
 * config.h — static park list. UUIDs match what PKJS recognises.
 */
#pragma once

#include <stdint.h>

typedef struct {
	const char *id;
	const char *name;
} park_config_t;

#define APP_NAME "Walt Disney World"
#define NUM_PARKS 4

extern const park_config_t g_parks[NUM_PARKS];

/* Auto-refresh interval (Pebble timer expects ms). */
#define REFRESH_INTERVAL_MS (5 * 60 * 1000)
