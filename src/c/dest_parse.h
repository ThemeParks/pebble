/*
 * dest_parse.h — destination-list wire parser + its static data store.
 *
 * Split out of dest_picker.c so the parser is host-compilable and unit
 * tested (dest_picker.c itself is all Pebble UI). Same pattern as
 * parkdata: one global store in BSS, parsing overwrites it in place.
 *
 * Wire format from PKJS (pipe-delimited, newline-separated):
 *   X|nogps                 — geolocation unavailable (no Nearby section)
 *   H|Section Name          — section header (Nearby, Europe, ...)
 *   D|id|name|parkCount|distKm   — destination row (distKm 0 = unknown)
 */
#pragma once

#include "config.h"

#ifdef MULTI_DESTINATION

#include <stdbool.h>
#include <stdint.h>

#define MAX_SECTIONS 8
/* The API serves ~155 destinations (mid-2026) and grows over time —
 * keep headroom so the tail of the list isn't silently dropped. */
#define MAX_DESTS 200
#define MAX_DEST_NAME 32
#define MAX_DEST_ID 40

typedef struct {
	char id[MAX_DEST_ID];
	char name[MAX_DEST_NAME];
	int park_count;
	int dist_km;         /* 0 or -1 if unknown/not-shown */
	int section;
} dest_entry_t;

typedef struct {
	char name[MAX_DEST_NAME];
	int first_row;       /* index into g_dests where this section starts */
	int count;
} dest_section_t;

extern dest_entry_t g_dests[MAX_DESTS];
extern dest_section_t g_dest_sections[MAX_SECTIONS];
extern int g_dest_count;
extern int g_dest_section_count;
extern int g_dest_nearby_section;   /* index of "Nearby", or -1 */
/* PKJS couldn't get a location fix — the picker shows a hint row in
 * place of the Nearby section so its absence reads as explained, not
 * broken. */
extern bool g_dest_no_location;

/* Parse a complete destination-list payload into the store above. */
void dest_parse(const char *data, uint16_t len);

/* DEST_PARKS payloads open with an "M|fixed" line when PKJS served them
 * because of the phone-side fixed-destination setting (rather than a
 * watch-side pick). Returns true when the marker is present and sets
 * *body_offset past the marker line, so the parks parser starts at the
 * first park row. No marker: false, offset 0. */
bool dest_parks_fixed_marker(const char *data, uint16_t len,
                             uint16_t *body_offset);

#endif /* MULTI_DESTINATION */
