/*
 * api.h — AppMessage transport for park data requests.
 *
 * Lifecycle: api_init() during app startup. api_fetch_park(id) sends a
 * request to PKJS; on response, the registered callback fires.
 */
#pragma once

#include <pebble.h>
#include <stdbool.h>

typedef void (*api_park_cb_t)(bool success);

/* Register the AppMessage handlers and inbox/outbox buffers. */
void api_init(void);
void api_deinit(void);

/* Set the callback that fires when a fetched park has been parsed (or failed). */
void api_set_callback(api_park_cb_t cb);

/* Send a request for park data. PKJS will respond with the pipe payload. */
void api_fetch_park(const char *park_id);

/* Ask PKJS for the current open/close hours of every known park. The
 * response is parsed into storage_set_park_hours() for each park. Used
 * once at startup so the park picker can show hours before any park
 * has been individually opened. */
void api_fetch_all_hours(void);

/* Returns true once the PKJS ready signal has been received. */
bool api_is_connected(void);

#ifdef MULTI_DESTINATION
/* Multi-destination callbacks: destination list + park list arrivals. */
typedef void (*api_dest_list_cb_t)(const char *data, uint16_t len);
typedef void (*api_dest_parks_cb_t)(const char *data, uint16_t len);

void api_set_dest_list_callback(api_dest_list_cb_t cb);
void api_set_dest_parks_callback(api_dest_parks_cb_t cb);

/* Ask PKJS to send the destination list (cached or fetched). */
void api_request_dest_list(void);

/* Tell PKJS which destination the user selected. PKJS responds with
 * DEST_PARKS_DATA containing the park list. */
void api_select_destination(const char *dest_id);
#endif
