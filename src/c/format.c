/*
 * format.c — pure display formatters. See format.h for the contracts.
 *
 * Kept free of <pebble.h> on purpose: scripts/unit_tests.py compiles
 * this file on the host and test_format.c exercises every branch.
 */
#include "format.h"
#include <stdio.h>
#include <string.h>

static void copy_through(const char *src, char *out, int outsz)
{
	if (outsz <= 0) return;
	if (!src) { out[0] = '\0'; return; }
	snprintf(out, outsz, "%s", src);
}

bool format_clock_time(const char *hhmm, bool use_24h, char *out, int outsz)
{
	if (outsz <= 0) return false;
	out[0] = '\0';
	/* Strict "HH:MM" parse. Anything else — including the "9 AM"-style
	 * strings persisted by older app versions — passes through so stale
	 * caches degrade gracefully instead of rendering blank. */
	if (!hhmm
	    || hhmm[0] < '0' || hhmm[0] > '9'
	    || hhmm[1] < '0' || hhmm[1] > '9'
	    || hhmm[2] != ':'
	    || hhmm[3] < '0' || hhmm[3] > '9'
	    || hhmm[4] < '0' || hhmm[4] > '9'
	    || hhmm[5] != '\0') {
		copy_through(hhmm, out, outsz);
		return false;
	}
	int hh = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
	int mm = (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
	if (hh > 23 || mm > 59) {
		copy_through(hhmm, out, outsz);
		return false;
	}

	if (use_24h) {
		snprintf(out, outsz, "%d:%02d", hh, mm);
		return true;
	}

	const char *ampm = (hh >= 12) ? "PM" : "AM";
	int h12 = hh % 12;
	if (h12 == 0) h12 = 12;
	if (mm == 0) snprintf(out, outsz, "%d %s", h12, ampm);
	else snprintf(out, outsz, "%d:%02d %s", h12, mm, ampm);
	return true;
}

void format_distance(int km, bool use_miles, char *out, int outsz)
{
	if (outsz <= 0) return;
	if (use_miles) {
		/* Integer conversion; the +500 rounds to nearest mile. */
		int mi = (km * 621 + 500) / 1000;
		if (mi < 1) snprintf(out, outsz, "< 1 mi");
		else snprintf(out, outsz, "%d mi", mi);
		return;
	}
	/* km < 2 covers the "rounded distance would read as 0-1 km" band the
	 * picker always displayed as "< 1 km". */
	if (km < 2) snprintf(out, outsz, "< 1 km");
	else snprintf(out, outsz, "%d km", km);
}

bool format_locale_uses_miles(const char *locale)
{
	if (!locale) return false;
	return strncmp(locale, "en_US", 5) == 0
	    || strncmp(locale, "en_GB", 5) == 0;
}

void format_age_suffix(uint32_t now_secs, uint32_t updated_secs,
                       char *out, int outsz)
{
	if (outsz <= 0) return;
	out[0] = '\0';
	if (updated_secs == 0 || now_secs < updated_secs) return;
	uint32_t age_s = now_secs - updated_secs;
	if (age_s < 120) return;   /* fresh — keep the header uncluttered */
	uint32_t age_m = age_s / 60;
	if (age_m < 60) snprintf(out, outsz, " · %um", (unsigned)age_m);
	else snprintf(out, outsz, " · %uh", (unsigned)(age_m / 60));
}
