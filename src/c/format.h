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
