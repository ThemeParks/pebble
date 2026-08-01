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

	if (eq == 4 && strncmp(seg, "line", 4) == 0) {
		/* "<text>|<expiryEpochSecs>", split on the LAST '|' (the text
		 * itself is guaranteed pipe-free by PKJS, but a last-split makes
		 * even a violated guarantee parse the expiry correctly — see
		 * settings_parse.h). Empty text is the explicit clear. */
		int last = -1;
		for (uint16_t i = 0; i < val_len; i++) {
			if (val[i] == '|') last = (int)i;
		}
		if (last < 0) return;              /* no expiry — malformed */

		const char *exp = val + last + 1;
		uint16_t exp_len = (uint16_t)(val_len - last - 1);
		/* Expiry: 1..10 digits, must fit uint32 — a garbled value must
		 * not wrap into a random future epoch. */
		if (exp_len == 0 || exp_len > 10) return;
		uint64_t expiry = 0;
		for (uint16_t d = 0; d < exp_len; d++) {
			if (exp[d] < '0' || exp[d] > '9') return;
			expiry = expiry * 10 + (uint64_t)(exp[d] - '0');
		}
		if (expiry > 0xffffffffu) return;

		uint16_t text_len = (uint16_t)last;
		/* Oversize text can't be ours (PKJS caps well under this) —
		 * reject rather than truncate mid-UTF-8. */
		if (text_len > SETTINGS_LINE_MAX - 1) return;
		memcpy(out->line_text, val, text_len);
		out->line_text[text_len] = '\0';
		out->line_expiry = (uint32_t)expiry;
		out->has_line = true;
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
