#pragma once

typedef void (*loading_retry_cb_t)(void);
typedef void (*loading_back_cb_t)(void);

/* Plain loading screen. BACK invokes `on_back` (pass NULL to make BACK
 * inert) so the user can abort a fetch that never completes. */
void loading_view_show(const char *park_name, loading_back_cb_t on_back);

/* Switch the loading screen to an error state. The label becomes
 * "Couldn't load\n<park_name>\nPress select to retry" and pressing
 * SELECT invokes `on_retry`. Press BACK to return to the picker. */
void loading_view_show_error(const char *park_name,
                             loading_retry_cb_t on_retry,
                             loading_back_cb_t on_back);

/* Show a connection-level error (PKJS not responding). SELECT retries,
 * BACK returns to picker. */
void loading_view_show_disconnected(loading_retry_cb_t on_retry,
                                    loading_back_cb_t on_back);

void loading_view_hide(void);
