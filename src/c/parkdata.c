/*
 * parkdata.c — pipe-delimited parser + the single static park data store.
 *
 * Wire format from PKJS (lines separated by '\n'):
 *   Line 0: "open|close" or ""         schedule strings
 *   Line 1: "<timestamp>"              lastUpdated (seconds)
 *   Lines:  "A|id|name|status|wait"    attractions (wait may be empty)
 *   Lines:  "S|id|name|status|t1,t2"   shows (comma-separated times)
 *   Lines:  "F|YYYY-MM-DD|open|close"  upcoming OPERATING days (optional)
 *
 * F| convention: PKJS sends every upcoming day it knows about INCLUDING
 * today. The glance publisher (main.c) keys today's hours off line 0 and
 * consumes F| entries only for dates strictly after today — storing all
 * of them here keeps the parser dumb and the dedupe in one place.
 *
 * All storage is in a single static struct (g_park), living in BSS, not
 * on any heap. Parsing overwrites the struct in place — there are no
 * per-parse allocations, so no leak across park switches.
 */

#include "parkdata.h"
#include <string.h>
#include <stdlib.h>

park_data_t g_park;

static void copy_bounded(char *dst, int dstSize, const char *src, int srcLen)
{
	if (srcLen > dstSize - 1) srcLen = dstSize - 1;
	if (srcLen < 0) srcLen = 0;
	memcpy(dst, src, srcLen);
	dst[srcLen] = '\0';
}

/* parse_int: accepts the span [s, s+len). Empty span returns -1 (signals
 * null/unknown wait time). Negative numbers are allowed but unused. */
static int parse_int_span(const char *s, int len)
{
	if (len <= 0) return -1;
	int v = 0;
	int i = 0;
	int neg = 0;
	if (s[0] == '-') { neg = 1; i = 1; }
	for (; i < len; i++) {
		char c = s[i];
		if (c < '0' || c > '9') break;
		v = v * 10 + (c - '0');
	}
	return neg ? -v : v;
}

void parkdata_clear(void)
{
	g_park.attraction_count = 0;
	g_park.show_count = 0;
	g_park.has_schedule = false;
	g_park.schedule_open[0] = '\0';
	g_park.schedule_close[0] = '\0';
	g_park.last_updated_secs = 0;
	g_park.future_day_count = 0;
	/* park_name preserved across clears so loading screen has it */
}

/* One "F|YYYY-MM-DD|open|close" line, [fp, nl) spanning the fields after
 * "F|". Strict date-shape check so a malformed line degrades to a
 * skipped entry instead of poisoning the glance queue; entries beyond
 * MAX_FUTURE_DAYS are dropped (the glance can only use ~a week anyway). */
static void parse_future_day(const char *raw, int fp, int nl)
{
	if (g_park.future_day_count >= MAX_FUTURE_DAYS) return;

	int p1 = fp;                                  /* end of date */
	while (p1 < nl && raw[p1] != '|') p1++;
	if (p1 >= nl) return;
	int p2 = p1 + 1;                              /* end of open */
	while (p2 < nl && raw[p2] != '|') p2++;
	if (p2 >= nl) return;

	int date_len = p1 - fp;
	int open_len = p2 - p1 - 1;
	int close_len = nl - p2 - 1;
	if (date_len != DATE_STR_LEN - 1 || open_len <= 0 || close_len <= 0)
		return;
	/* Strict "YYYY-MM-DD": digits with dashes at 4 and 7. The publisher
	 * strcmp-orders these against today's date, which only works when
	 * every entry has the exact shape. */
	for (int i = 0; i < DATE_STR_LEN - 1; i++) {
		char c = raw[fp + i];
		if (i == 4 || i == 7) {
			if (c != '-') return;
		} else if (c < '0' || c > '9') {
			return;
		}
	}

	future_day_t *f = &g_park.future_days[g_park.future_day_count];
	copy_bounded(f->date, DATE_STR_LEN, raw + fp, date_len);
	copy_bounded(f->open, MAX_HOURS_LEN, raw + p1 + 1, open_len);
	copy_bounded(f->close, MAX_HOURS_LEN, raw + p2 + 1, close_len);
	g_park.future_day_count++;
}

bool parkdata_all_closed(void)
{
	/* Drives the "Closed" header state in the park view: only claim the
	 * park is closed when we have data and not a single ride operates.
	 * An empty list means "no data", not "closed". */
	if (g_park.attraction_count == 0) return false;
	for (uint8_t i = 0; i < g_park.attraction_count; i++) {
		if (g_park.attractions[i].status == 'O') return false;
	}
	return true;
}

void parkdata_set_park_name(const char *name)
{
	if (!name) { g_park.park_name[0] = '\0'; return; }
	copy_bounded(g_park.park_name, sizeof(g_park.park_name), name, strlen(name));
}

bool parkdata_parse(const char *raw, uint16_t raw_len)
{
	parkdata_clear();
	if (!raw || raw_len == 0) return false;

	int pos = 0;

	/* --- Line 0: schedule ------------------------------------------------- */
	int nl = pos;
	while (nl < raw_len && raw[nl] != '\n') nl++;
	{
		int line_len = nl - pos;
		if (line_len > 0) {
			int pipe = pos;
			while (pipe < nl && raw[pipe] != '|') pipe++;
			if (pipe < nl) {
				copy_bounded(g_park.schedule_open, MAX_HOURS_LEN,
				             raw + pos, pipe - pos);
				copy_bounded(g_park.schedule_close, MAX_HOURS_LEN,
				             raw + pipe + 1, nl - pipe - 1);
				g_park.has_schedule = true;
			}
		}
	}
	pos = nl + 1;

	/* --- Line 1: timestamp ------------------------------------------------ */
	nl = pos;
	while (nl < raw_len && raw[nl] != '\n') nl++;
	{
		int v = parse_int_span(raw + pos, nl - pos);
		g_park.last_updated_secs = (v < 0) ? 0 : (uint32_t)v;
	}
	pos = nl + 1;

	/* --- Remaining lines: A|... or S|... ---------------------------------- */
	while (pos < raw_len) {
		nl = pos;
		while (nl < raw_len && raw[nl] != '\n') nl++;

		if (nl - pos < 2) { pos = nl + 1; continue; }

		char type = raw[pos];
		int fp = pos + 2;  /* skip "A|", "S|" or "F|" */

		/* Future-day lines have 3 fields, not the 4 the generic scan
		 * below expects — branch off before it rejects them. */
		if (type == 'F' && raw[pos + 1] == '|') {
			parse_future_day(raw, fp, nl);
			pos = nl + 1;
			continue;
		}

		/* Field 1: id */
		int p1 = fp;
		while (p1 < nl && raw[p1] != '|') p1++;
		if (p1 >= nl) { pos = nl + 1; continue; }

		/* Field 2: name */
		int p2 = p1 + 1;
		while (p2 < nl && raw[p2] != '|') p2++;
		if (p2 >= nl) { pos = nl + 1; continue; }

		/* Field 3: status */
		int p3 = p2 + 1;
		while (p3 < nl && raw[p3] != '|') p3++;
		if (p3 >= nl) { pos = nl + 1; continue; }

		const char *id_start = raw + fp;
		int id_len = p1 - fp;
		const char *name_start = raw + p1 + 1;
		int name_len = p2 - p1 - 1;
		char status_char = (p3 - p2 - 1 >= 1) ? raw[p2 + 1] : '?';
		const char *val_start = raw + p3 + 1;
		int val_len = nl - p3 - 1;

		if (type == 'A' && g_park.attraction_count >= MAX_ATTRACTIONS) {
			APP_LOG(APP_LOG_LEVEL_WARNING, "truncated: >%d attractions", MAX_ATTRACTIONS);
			pos = nl + 1; continue;
		} else if (type == 'S' && g_park.show_count >= MAX_SHOWS) {
			APP_LOG(APP_LOG_LEVEL_WARNING, "truncated: >%d shows", MAX_SHOWS);
			pos = nl + 1; continue;
		}

		if (type == 'A' && g_park.attraction_count < MAX_ATTRACTIONS) {
			attraction_t *a = &g_park.attractions[g_park.attraction_count];
			copy_bounded(a->id, MAX_ID_LEN, id_start, id_len);
			copy_bounded(a->name, MAX_NAME_LEN, name_start, name_len);
			a->status = status_char;
			a->wait = parse_int_span(val_start, val_len);
			g_park.attraction_count++;
		} else if (type == 'S' && g_park.show_count < MAX_SHOWS) {
			show_t *s = &g_park.shows[g_park.show_count];
			copy_bounded(s->id, MAX_ID_LEN, id_start, id_len);
			copy_bounded(s->name, MAX_NAME_LEN, name_start, name_len);
			s->status = status_char;
			/* Split comma-separated times into pre-allocated string slots */
			s->time_count = 0;
			int tp = 0;
			while (tp < val_len && s->time_count < MAX_TIMES_PER_SHOW) {
				int comma = tp;
				while (comma < val_len && val_start[comma] != ',') comma++;
				copy_bounded(s->times[s->time_count], MAX_TIME_LEN,
				             val_start + tp, comma - tp);
				s->time_count++;
				tp = comma + 1;
			}
			g_park.show_count++;
		}

		pos = nl + 1;
	}

	return true;
}
