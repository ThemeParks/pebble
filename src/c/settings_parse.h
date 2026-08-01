/*
 * settings_parse.h — parser for the SETTINGS_DATA wire payload.
 *
 * Pure (no pebble.h) so the host unit tests can compile it directly;
 * api.c applies the parsed fields to storage.
 *
 * Wire format from PKJS (lib.encodeSettingsPayload): semicolon-separated
 * key=value segments, e.g. "glance=1" or
 * "glance=1;line=MK 45m · EP 30m|1754081234". Unknown segments are
 * ignored so a newer phone app can talk to an older watch binary;
 * malformed segments never invalidate the well-formed ones.
 *
 * The "line" value is the packed My Parks glance line followed by its
 * expiry: "<text>|<expiryEpochSecs>", split on the LAST '|'. The text is
 * raw UTF-8, no escaping: PKJS (lib.buildGlanceLine) guarantees it never
 * contains ';' (would split the segment) or '=' or '|' — park codes are
 * [A-Z0-9], waits render as "<N>m", the separator is " · " (the UTF-8
 * middle dot, which is just opaque bytes here). The last-pipe split
 * plus an all-digits expiry check keeps even a violated guarantee from
 * parsing into garbage. Empty text with a valid expiry is the explicit
 * "clear the stored line" signal ("line=|0").
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Text cap incl. NUL. PKJS budgets 60 chars, but the three " · "
 * separators are 3 UTF-8 bytes each — 80 gives comfortable slack.
 * Anything longer is not ours and parses as malformed (absent).
 * Keep in sync with GLANCE_LINE_MAX in storage.h. */
#define SETTINGS_LINE_MAX 80

typedef struct {
	/* has_* gates each field: absent/malformed keys leave the watch's
	 * current persisted value untouched instead of forcing a default. */
	bool has_glance;
	bool glance;
	/* Packed My Parks glance line. has_line with line_text[0]=='\0'
	 * means "clear". line_expiry is epoch seconds. */
	bool has_line;
	char line_text[SETTINGS_LINE_MAX];
	uint32_t line_expiry;
} settings_msg_t;

/* Parse `len` bytes of payload (need not be NUL-terminated) into *out.
 * Always fully initialises *out; NULL/empty input parses to no fields. */
void settings_parse(const char *data, uint16_t len, settings_msg_t *out);
