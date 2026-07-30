/*
 * parkdata.h — public interface for the static park data store + parser.
 */
#pragma once

#include <pebble.h>
#include <stdint.h>

#define MAX_ATTRACTIONS 100
#define MAX_SHOWS 40
#define MAX_TIMES_PER_SHOW 4
#define MAX_ID_LEN 12      /* 8 hex chars + slack */
/* Names are stored up to 36 chars (+null) so the renderer can decide how
 * much to show based on the row's actual available width — a wide middle
 * row on gabbro or the full-width stacked Shows row can display more
 * than the narrow-waits column. PKJS caps at 36 chars to match. */
#define MAX_NAME_LEN 40
#define MAX_TIME_LEN 12    /* "10:30 PM" + slack */
#define MAX_HOURS_LEN 12
#define MAX_FUTURE_DAYS 7  /* ~a week of glance slices; AppGlance caps at 8 */
#define DATE_STR_LEN 11    /* "YYYY-MM-DD" + NUL */

typedef struct {
	char id[MAX_ID_LEN];
	char name[MAX_NAME_LEN];
	char status;             /* 'O', 'C', 'D', 'R' */
	int wait;                /* -1 == null/unknown */
} attraction_t;

typedef struct {
	char id[MAX_ID_LEN];
	char name[MAX_NAME_LEN];
	char status;
	char times[MAX_TIMES_PER_SHOW][MAX_TIME_LEN];
	uint8_t time_count;
} show_t;

/* One upcoming OPERATING day from the wire's F| lines — feeds the
 * multi-day AppGlance slice queue (main.c publish_glance). */
typedef struct {
	char date[DATE_STR_LEN];    /* "YYYY-MM-DD", park-local */
	char open[MAX_HOURS_LEN];   /* wire "HH:MM" */
	char close[MAX_HOURS_LEN];
} future_day_t;

typedef struct {
	attraction_t attractions[MAX_ATTRACTIONS];
	show_t shows[MAX_SHOWS];
	uint8_t attraction_count;
	uint8_t show_count;
	char schedule_open[MAX_HOURS_LEN];
	char schedule_close[MAX_HOURS_LEN];
	bool has_schedule;
	future_day_t future_days[MAX_FUTURE_DAYS];
	uint8_t future_day_count;
	uint32_t last_updated_secs;  /* seconds since epoch */
	char park_name[24];
} park_data_t;

/* The one global store. Accessed everywhere read-only after a parse. */
extern park_data_t g_park;

/* Reset the store to empty (release before fetching new park). */
void parkdata_clear(void);

/* Parse a pipe-delimited payload into g_park. Returns 1 on success. */
bool parkdata_parse(const char *raw, uint16_t raw_len);

/* Set the park name (called by api.c after parse using the requested name). */
void parkdata_set_park_name(const char *name);

/* True iff we have attraction data and none of it is operating — the
 * park-closed header state. Empty data reads as "unknown", not closed. */
bool parkdata_all_closed(void);
