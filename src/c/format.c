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

bool format_hours_current(int now_min, int open_min, int close_min)
{
	if (now_min < 0 || now_min >= 24 * 60) return false;
	if (open_min < 0 || open_min >= 24 * 60) return false;
	if (close_min < 0 || close_min >= 24 * 60) return false;
	if (close_min < open_min) {
		/* Past-midnight close (open 09:00, close 01:00): "current" spans
		 * the operating evening (now >= open) and the small hours before
		 * close (now < close). The dead zone in between reads as expired
		 * — with only minutes-of-day we can't tell 02:00 "just after last
		 * night's close" from "before today's open", and a stale glance
		 * is worse than none. */
		return now_min >= open_min || now_min < close_min;
	}
	/* Normal day: informative right up to close — including pre-open,
	 * where "opens at 9" is exactly what the user wants to see. */
	return now_min < close_min;
}

void format_glance_countdown(const char *park_name, const char *verb,
                             uint32_t epoch, char *out, int outsz)
{
	if (outsz <= 0) return;
	out[0] = '\0';
	if (!verb || !verb[0]) return;
	/* The {time_until(...)|format(...)} braces are template syntax the
	 * launcher evaluates at render time — everything else is literal.
	 * Epoch as %lu: watch time_t is 32-bit and Pebble's tiny printf has
	 * no %lld, so uint32_t in and unsigned long through the formatter
	 * works on both the watch and the 64-bit test host. */
	const char *park =
		(park_name && park_name[0]) ? park_name : NULL;
	if (park)
		snprintf(out, outsz,
		         "%s · %s in {time_until(%lu)|format('%%aR')}",
		         park, verb, (unsigned long)epoch);
	else
		snprintf(out, outsz,
		         "%s in {time_until(%lu)|format('%%aR')}",
		         verb, (unsigned long)epoch);
}

int format_glance_line(const glance_input_t *in, bool use_24h,
                       char *out, int outsz)
{
	if (outsz <= 0) return GLANCE_RUNG_NONE;
	out[0] = '\0';
	if (!in) return GLANCE_RUNG_NONE;

	const char *park =
		(in->park_name && in->park_name[0]) ? in->park_name : NULL;

	/* Rung 1: park hours. The caller decides "current" (it has real
	 * dates; we don't) — we just render. Legacy cached strings ("9 AM")
	 * pass through format_clock_time unchanged, which is still readable,
	 * so no parse check here. */
	if (in->has_schedule && in->hours_current) {
		char open_s[16], close_s[16];
		format_clock_time(in->open_hhmm, use_24h, open_s, sizeof open_s);
		format_clock_time(in->close_hhmm, use_24h, close_s, sizeof close_s);
		if (open_s[0] && close_s[0]) {
			if (park) snprintf(out, outsz, "%s · %s - %s",
			                   park, open_s, close_s);
			else snprintf(out, outsz, "%s - %s", open_s, close_s);
			return GLANCE_RUNG_HOURS;
		}
	}

	/* Rung 2: headline wait — only while fresh (the caller passes the
	 * data age; we enforce the cutoff too, belt and braces). */
	if (in->ride_name && in->ride_name[0] && in->wait_mins >= 0
	    && in->wait_age_secs < GLANCE_WAIT_MAX_AGE_SECS) {
		if (park) snprintf(out, outsz, "%s %dm · %s",
		                   in->ride_name, in->wait_mins, park);
		else snprintf(out, outsz, "%s %dm", in->ride_name, in->wait_mins);
		return GLANCE_RUNG_WAIT;
	}

	/* Rung 3: nothing usable — the caller publishes no slice and the
	 * launcher shows the plain app name. */
	return GLANCE_RUNG_NONE;
}
