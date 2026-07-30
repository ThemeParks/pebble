/*
 * format.h — pure display formatters (no Pebble SDK dependency).
 *
 * Everything here is host-compilable so it can be unit tested off-watch.
 * The UI layers feed in the user preference flags (24h clock, miles)
 * from the SDK (clock_is_24h_style(), i18n_get_system_locale()) — this
 * module never asks the SDK itself.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Render a wire-format time ("HH:MM", zero-padded 24h from PKJS) for
 * display. 24h mode: "9:00" / "21:30" (no leading zero on the hour).
 * 12h mode: "9 AM" / "9:30 PM" (minutes elided when :00). Anything that
 * doesn't parse as a valid HH:MM — including legacy persisted strings
 * like "9 AM" written by older app versions — is copied through
 * unchanged (truncated to outsz) and returns false. */
bool format_clock_time(const char *hhmm, bool use_24h, char *out, int outsz);

/* Render a distance for display. km mode: "< 1 km" under 2 km, else
 * "N km" (matches the dest picker's original behaviour). miles mode:
 * integer-rounded conversion, "< 1 mi" when it rounds below 1. */
void format_distance(int km, bool use_miles, char *out, int outsz);

/* Whether a system locale string implies miles for distances. Dumb
 * prefix check on the countries that signpost in miles (US, UK) — a
 * wrong guess here still shows a correct distance, just in the less
 * familiar unit. */
bool format_locale_uses_miles(const char *locale);

/* Staleness suffix for the park-view header: "" when the timestamp is
 * unknown, fresh (<2 min) or in the future (clock skew); otherwise
 * " · Nm" under an hour, " · Nh" beyond. */
void format_age_suffix(uint32_t now_secs, uint32_t updated_secs,
                       char *out, int outsz);

/* --- AppGlance launcher subtitle ------------------------------------- */

/* Wait data older than this must not reach the launcher — a stale wait
 * on the glance would be worse than showing nothing. Shared with main.c,
 * which also uses it as the wait slice's expiration horizon. */
#define GLANCE_WAIT_MAX_AGE_SECS (30 * 60)

/* Which rung of the glance priority ladder produced the line. */
enum {
	GLANCE_RUNG_NONE  = 0,  /* nothing usable — publish no slice */
	GLANCE_RUNG_HOURS = 1,  /* "<Park> · <open> - <close>" */
	GLANCE_RUNG_WAIT  = 2,  /* "<Ride> <N>m · <Park>" */
};

typedef struct {
	const char *park_name;   /* NULL/"" → line renders without it */
	bool has_schedule;       /* open/close strings hold today's hours */
	const char *open_hhmm;   /* wire "HH:MM" (legacy strings pass through) */
	const char *close_hhmm;
	bool hours_current;      /* caller's call: close is still in the future */
	const char *ride_name;   /* headline attraction, NULL/"" if none */
	int wait_mins;           /* its wait, -1 == none */
	uint32_t wait_age_secs;  /* age of the wait data (now - last update) */
} glance_input_t;

/* Compose the glance subtitle per the priority ladder: current hours
 * beat a fresh headline wait beats nothing. Returns the GLANCE_RUNG_*
 * used; out is "" (and NONE returned) when no rung applies. Truncates
 * safely into outsz. */
int format_glance_line(const glance_input_t *in, bool use_24h,
                       char *out, int outsz);

/* Compose an AppGlance subtitle countdown template:
 *   "<Park> · <verb> in {time_until(<epoch>)|format('%aR')}"
 * The launcher evaluates the template every time it renders the glance,
 * so the text stays continuously accurate with zero app execution.
 * '%aR' is the abbreviated hours+minutes span ("2 hr 4 min", "45 min")
 * — the readable choice for the multi-hour spans park days produce
 * (syntax per the archived SDK AppGlance guide; see
 * docs/future/app-glance.md). Empty/NULL park drops the prefix instead
 * of rendering a dangling " · ". Truncates safely into outsz — but a
 * truncated template renders literally, so callers must size out for
 * the whole line (main.c uses 96-byte slots). */
void format_glance_countdown(const char *park_name, const char *verb,
                             uint32_t epoch, char *out, int outsz);

/* Are park hours still worth showing, given minutes-since-midnight?
 * True from midnight up to close (pre-open hours are informative), with
 * past-midnight closes (close < open) handled — see the impl comment.
 * Out-of-range inputs (outside 0..1439) return false. */
bool format_hours_current(int now_min, int open_min, int close_min);
