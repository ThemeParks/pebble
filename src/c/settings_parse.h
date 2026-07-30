/*
 * settings_parse.h — parser for the SETTINGS_DATA wire payload.
 *
 * Pure (no pebble.h) so the host unit tests can compile it directly;
 * api.c applies the parsed fields to storage.
 *
 * Wire format from PKJS (lib.encodeSettingsPayload): semicolon-separated
 * key=value segments, e.g. "glance=1" or "glance=0;future=x". Unknown
 * segments are ignored so a newer phone app can talk to an older watch
 * binary; malformed segments never invalidate the well-formed ones.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	/* has_* gates each field: absent/malformed keys leave the watch's
	 * current persisted value untouched instead of forcing a default. */
	bool has_glance;
	bool glance;
} settings_msg_t;

/* Parse `len` bytes of payload (need not be NUL-terminated) into *out.
 * Always fully initialises *out; NULL/empty input parses to no fields. */
void settings_parse(const char *data, uint16_t len, settings_msg_t *out);
