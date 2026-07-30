/*
 * settings_parse.c — parser for the SETTINGS_DATA wire payload.
 *
 * See settings_parse.h for the format. Defensive by design: this runs
 * on whatever bytes the phone sends, and a settings push must never be
 * able to corrupt state — anything not exactly "known key = known
 * value" is skipped.
 */

#include "settings_parse.h"
#include <string.h>

/* One "key=value" segment (seg..seg+seg_len, not NUL-terminated). */
static void parse_segment(const char *seg, uint16_t seg_len,
                          settings_msg_t *out)
{
	uint16_t eq = 0;
	while (eq < seg_len && seg[eq] != '=') eq++;
	if (eq == 0 || eq >= seg_len) return;   /* no key or no '=' */

	const char *val = seg + eq + 1;
	uint16_t val_len = seg_len - eq - 1;

	if (eq == 6 && strncmp(seg, "glance", 6) == 0) {
		/* Value must be exactly "0" or "1" — a garbled value must not
		 * flip the flag either way. */
		if (val_len == 1 && (val[0] == '0' || val[0] == '1')) {
			out->has_glance = true;
			out->glance = val[0] == '1';
		}
	}
	/* Unknown keys: ignored (forward compatibility). */
}

void settings_parse(const char *data, uint16_t len, settings_msg_t *out)
{
	memset(out, 0, sizeof(*out));
	if (!data) return;

	uint16_t pos = 0;
	while (pos < len) {
		uint16_t end = pos;
		while (end < len && data[end] != ';') end++;
		if (end > pos) parse_segment(data + pos, end - pos, out);
		pos = end + 1;
	}
}
