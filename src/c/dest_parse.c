/*
 * dest_parse.c — destination-list wire parser. Extracted verbatim from
 * dest_picker.c so it can be host-compiled for unit tests; see
 * dest_parse.h for the wire format.
 */
#include "dest_parse.h"

#ifdef MULTI_DESTINATION

#include <pebble.h>
#include <string.h>
#include <stdlib.h>

dest_entry_t g_dests[MAX_DESTS];
dest_section_t g_dest_sections[MAX_SECTIONS];
int g_dest_count = 0;
int g_dest_section_count = 0;
int g_dest_nearby_section = -1;
bool g_dest_no_location = false;

void dest_parse(const char *data, uint16_t len)
{
	g_dest_count = 0;
	g_dest_section_count = 0;
	g_dest_nearby_section = -1;
	g_dest_no_location = false;
	int pos = 0;
	int current_section = -1;

	while (pos < len && g_dest_count < MAX_DESTS) {
		/* Find end of line */
		int nl = pos;
		while (nl < len && data[nl] != '\n') nl++;
		int line_len = nl - pos;

		if (line_len < 2) {
			pos = nl + 1;
			continue;
		}

		if (data[pos] == 'X' && data[pos + 1] == '|') {
			/* Location-unavailable marker from PKJS. */
			g_dest_no_location = true;
		} else if (line_len >= 3 && data[pos] == 'H' && data[pos + 1] == '|') {
			/* Section header */
			if (g_dest_section_count < MAX_SECTIONS) {
				dest_section_t *sec = &g_dest_sections[g_dest_section_count];
				int name_len = line_len - 2;
				if (name_len > MAX_DEST_NAME - 1) name_len = MAX_DEST_NAME - 1;
				memcpy(sec->name, data + pos + 2, name_len);
				sec->name[name_len] = '\0';
				sec->first_row = g_dest_count;
				sec->count = 0;
				if (!strcmp(sec->name, "Nearby")) {
					g_dest_nearby_section = g_dest_section_count;
				}
				current_section = g_dest_section_count;
				g_dest_section_count++;
			}
		} else if (line_len >= 3 && data[pos] == 'D' && data[pos + 1] == '|') {
			/* Destination row: D|id|name|parkCount|distKm */
			const char *p = data + pos + 2;
			const char *end = data + nl;
			const char *fs = p;
			int fi = 0;
			const char *fields[4] = {NULL, NULL, NULL, NULL};
			int flens[4] = {0, 0, 0, 0};
			for (const char *c = p; c <= end && fi < 4; c++) {
				if (c == end || *c == '|') {
					fields[fi] = fs;
					flens[fi] = (int)(c - fs);
					fi++;
					fs = c + 1;
				}
			}
			if (fi >= 3) {
				dest_entry_t *d = &g_dests[g_dest_count];
				int id_len = flens[0] < MAX_DEST_ID - 1 ? flens[0] : MAX_DEST_ID - 1;
				memcpy(d->id, fields[0], id_len);
				d->id[id_len] = '\0';
				int nm_len = flens[1] < MAX_DEST_NAME - 1 ? flens[1] : MAX_DEST_NAME - 1;
				memcpy(d->name, fields[1], nm_len);
				d->name[nm_len] = '\0';
				char tmp[8];
				int c2_len = flens[2] < 7 ? flens[2] : 7;
				memcpy(tmp, fields[2], c2_len);
				tmp[c2_len] = '\0';
				d->park_count = atoi(tmp);
				if (fi >= 4) {
					int c3_len = flens[3] < 7 ? flens[3] : 7;
					memcpy(tmp, fields[3], c3_len);
					tmp[c3_len] = '\0';
					d->dist_km = atoi(tmp);
				} else {
					d->dist_km = 0;
				}
				d->section = current_section;
				g_dest_count++;
				if (current_section >= 0 && current_section < g_dest_section_count) {
					g_dest_sections[current_section].count++;
				}
			}
		}
		pos = nl + 1;
	}
	APP_LOG(APP_LOG_LEVEL_INFO, "dest_picker: %d dests in %d sections",
	        g_dest_count, g_dest_section_count);
}

#endif /* MULTI_DESTINATION */
