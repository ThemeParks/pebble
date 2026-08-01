/*
 * api.c — AppMessage send/receive.
 *
 * Inbox buffer is sized for the largest pipe payload (~8KB worst case for
 * 100 attractions × ~55 chars + 40 shows × ~65 chars + headers). The
 * Pebble framework copies inbound message bytes into this buffer; we then
 * hand the bytes to parkdata_parse() and forget them.
 */

#include "config.h"
#include "api.h"
#include "parkdata.h"
#include "settings_parse.h"
#include "storage.h"
#include "message_keys.auto.h"
#include <string.h>
#include <stdlib.h>

#define INBOX_SIZE 8192
#define OUTBOX_SIZE 256

/* Payloads bigger than the inbox (the full destination list is ~11KB)
 * arrive split into newline-boundary chunks tagged with CHUNK_INDEX /
 * CHUNK_MORE. They're reassembled here before dispatch. The buffer is
 * heap-allocated only while a transfer is in flight. */
#define ASM_BUF_SIZE 16384

/* If outbox_failed fires (e.g. PKJS not ready yet), retry after this delay. */
#define RETRY_DELAY_MS 500
#define MAX_RETRIES 10

static api_park_cb_t s_callback = NULL;
static char s_pending_park_id[40];
/* Manual ("force") refreshes tell PKJS to bypass its fresh-cache check.
 * The flag rides alongside s_pending_park_id through queueing/retries. */
static bool s_pending_force = false;
static int s_retries_left = 0;
static AppTimer *s_retry_timer = NULL;

/* PKJS readiness — set true when we receive the first inbox message from
 * PKJS (which is its empty-error ready signal). Until then, queue any
 * fetch requests and flush them once ready. */
static bool s_pkjs_ready = false;
static bool s_fetch_queued = false;
static bool s_all_hours_queued = false;

/* Heartbeat: detect PKJS unresponsiveness at startup. */
#define READY_TIMEOUT_MS 10000
#define HEARTBEAT_MAX_INTERVAL_MS 60000

static AppTimer *s_ready_timer = NULL;
static AppTimer *s_heartbeat_timer = NULL;
static uint32_t s_heartbeat_interval = READY_TIMEOUT_MS;

#ifdef MULTI_DESTINATION
static api_dest_list_cb_t s_dest_list_cb = NULL;
static api_dest_parks_cb_t s_dest_parks_cb = NULL;
static bool s_dest_list_queued = false;
static char s_pending_dest_id[40] = {0};  /* empty string = nothing pending */
static AppTimer *s_dest_list_retry_timer = NULL;
#endif

static void send_request(void);
static void retry_tick(void *ctx)
{
	s_retry_timer = NULL;
	send_request();
}

/* Parse the "pid|open|close\n..." payload PKJS sends in response to an
 * all-hours request. Each line becomes a storage_set_park_hours() call.
 * We write in place from `raw`; the only mutation is NUL-terminating at
 * pipes, which is fine since the buffer goes out of scope after. */
static void parse_all_hours(char *raw, uint16_t len)
{
	uint16_t pos = 0;
	int count = 0;
	while (pos < len) {
		int eol = pos;
		while (eol < len && raw[eol] != '\n') eol++;

		int p1 = pos;
		while (p1 < eol && raw[p1] != '|') p1++;
		int p2 = p1 + 1;
		while (p2 < eol && raw[p2] != '|') p2++;
		if (p1 < eol && p2 < eol) {
			raw[p1] = '\0';
			raw[p2] = '\0';
			raw[eol] = '\0';
			const char *pid = raw + pos;
			const char *open = raw + p1 + 1;
			const char *close = raw + p2 + 1;
			storage_set_park_hours(pid, open, close);
			count++;
		}
		pos = eol + 1;
	}
	APP_LOG(APP_LOG_LEVEL_INFO, "cached hours for %d park(s)", count);
}

static char *s_asm_buf = NULL;
static uint16_t s_asm_len = 0;
static uint32_t s_asm_key = 0;
static int32_t s_asm_next = 0;   /* next chunk index we expect */

static void asm_reset(void)
{
	if (s_asm_buf) {
		free(s_asm_buf);
		s_asm_buf = NULL;
	}
	s_asm_len = 0;
	s_asm_key = 0;
	s_asm_next = 0;
}

/* Route a complete (reassembled or single-message) payload to its
 * consumer, identified by which message key carried it. */
static void dispatch_payload(uint32_t key, char *buf, uint16_t len)
{
	if (key == MESSAGE_KEY_ALL_HOURS_DATA) {
		/* All-hours response — parse + persist, no callback needed */
		parse_all_hours(buf, len);
		return;
	}
	if (key == MESSAGE_KEY_SETTINGS_DATA) {
		/* Phone-side settings push ("glance=0|1;..."). Fields the
		 * payload doesn't carry keep their persisted values. */
		settings_msg_t sm;
		settings_parse(buf, len, &sm);
		if (sm.has_glance) {
			storage_set_glance_enabled(sm.glance);
			APP_LOG(APP_LOG_LEVEL_INFO, "settings: glance %s",
			        sm.glance ? "on" : "off");
		}
		if (sm.has_line) {
			/* Empty text clears the stored line (storage contract). */
			storage_set_glance_line(sm.line_text, sm.line_expiry);
			APP_LOG(APP_LOG_LEVEL_INFO, "settings: line %s",
			        sm.line_text[0] ? sm.line_text : "(clear)");
		}
		return;
	}
#ifdef MULTI_DESTINATION
	if (key == MESSAGE_KEY_DEST_LIST_DATA) {
		/* List delivered — stop the re-request watchdog. An empty payload
		 * (upstream API hiccup) keeps it alive: parsing zero destinations
		 * is harmless, but cancelling would strand the picker on
		 * "Loading destinations..." with no recovery. */
		if (len > 0 && s_dest_list_retry_timer) {
			app_timer_cancel(s_dest_list_retry_timer);
			s_dest_list_retry_timer = NULL;
		}
		if (s_dest_list_cb) s_dest_list_cb(buf, len);
		return;
	}
	if (key == MESSAGE_KEY_DEST_PARKS_DATA) {
		/* Parks arriving means the request chain concluded — in fixed-
		 * destination mode PKJS answers a dest-LIST request with the
		 * fixed destination's parks and no DEST_LIST_DATA ever comes, so
		 * the list watchdog must stand down here too or it would
		 * re-request (and re-receive these parks) forever. */
		if (s_dest_list_retry_timer) {
			app_timer_cancel(s_dest_list_retry_timer);
			s_dest_list_retry_timer = NULL;
		}
		if (s_dest_parks_cb) s_dest_parks_cb(buf, len);
		return;
	}
#endif
	if (key == MESSAGE_KEY_PARK_DATA_JSON) {
		APP_LOG(APP_LOG_LEVEL_INFO, "recv %u bytes", (unsigned)len);
		s_pending_park_id[0] = '\0';
		bool ok = parkdata_parse(buf, len);
		if (s_callback) s_callback(ok);
	}
}

static void inbox_received_handler(DictionaryIterator *iter, void *context)
{
	Tuple *err = dict_find(iter, MESSAGE_KEY_ERROR_MSG);

	/* Find whichever data key this message carries (at most one). */
	uint32_t data_key = MESSAGE_KEY_PARK_DATA_JSON;
	Tuple *data = dict_find(iter, MESSAGE_KEY_PARK_DATA_JSON);
	if (!data) {
		data = dict_find(iter, MESSAGE_KEY_ALL_HOURS_DATA);
		data_key = MESSAGE_KEY_ALL_HOURS_DATA;
	}
	if (!data) {
		data = dict_find(iter, MESSAGE_KEY_SETTINGS_DATA);
		data_key = MESSAGE_KEY_SETTINGS_DATA;
	}
#ifdef MULTI_DESTINATION
	if (!data) {
		data = dict_find(iter, MESSAGE_KEY_DEST_LIST_DATA);
		data_key = MESSAGE_KEY_DEST_LIST_DATA;
	}
	if (!data) {
		data = dict_find(iter, MESSAGE_KEY_DEST_PARKS_DATA);
		data_key = MESSAGE_KEY_DEST_PARKS_DATA;
	}
#endif

	/* The first message from PKJS is its empty-error ready signal:
	 *   {ERROR_MSG: ""}. Use this as the gate to start sending requests. */
	if (!s_pkjs_ready) {
		if (s_ready_timer) {
			app_timer_cancel(s_ready_timer);
			s_ready_timer = NULL;
		}
		if (s_heartbeat_timer) {
			app_timer_cancel(s_heartbeat_timer);
			s_heartbeat_timer = NULL;
		}
		s_heartbeat_interval = READY_TIMEOUT_MS;
		s_pkjs_ready = true;
		if (s_fetch_queued) {
			s_fetch_queued = false;
			send_request();
		}
		if (s_all_hours_queued) {
			s_all_hours_queued = false;
			api_fetch_all_hours();
		}
#ifdef MULTI_DESTINATION
		if (s_dest_list_queued) {
			s_dest_list_queued = false;
			api_request_dest_list();
		}
		if (s_pending_dest_id[0]) {
			char tmp[40];
			strncpy(tmp, s_pending_dest_id, sizeof(tmp) - 1);
			tmp[sizeof(tmp) - 1] = '\0';
			s_pending_dest_id[0] = '\0';
			api_select_destination(tmp);
		}
#endif
	}

	if (err && err->length > 1) {
		APP_LOG(APP_LOG_LEVEL_WARNING, "PKJS error: %s", err->value->cstring);
		/* Fetch concluded — clear it so a later outbox failure on an
		 * unrelated message can't trigger a phantom park retry. */
		s_pending_park_id[0] = '\0';
		if (s_callback) s_callback(false);
		return;
	}

	if (!data) {
		/* Initial ready handshake — nothing more to do */
		return;
	}

	Tuple *chunk_idx = dict_find(iter, MESSAGE_KEY_CHUNK_INDEX);
	Tuple *chunk_more = dict_find(iter, MESSAGE_KEY_CHUNK_MORE);
	if (!chunk_idx) {
		/* Unchunked payload — dispatch straight from the inbox buffer. */
		dispatch_payload(data_key, data->value->cstring,
		                 data->length > 0 ? data->length - 1 : 0);
		return;
	}

	int32_t idx = chunk_idx->value->int32;
	bool more = chunk_more && chunk_more->value->int32 != 0;

	if (idx == 0) {
		asm_reset();
		s_asm_buf = malloc(ASM_BUF_SIZE);
		if (!s_asm_buf) {
			APP_LOG(APP_LOG_LEVEL_ERROR, "chunk buffer alloc failed");
			return;
		}
		s_asm_buf[0] = '\0';
		s_asm_key = data_key;
	}
	if (!s_asm_buf || s_asm_key != data_key || idx > s_asm_next) {
		/* Chunk without a matching in-flight transfer (e.g. first chunk
		 * was dropped, or transfers interleaved) — drop the sequence;
		 * PKJS-side retries / the fetch watchdog handle recovery. */
		APP_LOG(APP_LOG_LEVEL_WARNING, "chunk %d out of sequence", (int)idx);
		asm_reset();
		return;
	}
	if (idx < s_asm_next) {
		/* Retransmit of a chunk we already appended (the phone missed our
		 * ack) — ignore it rather than duplicating the bytes. */
		return;
	}

	/* Chunks are split at newline boundaries with the separator elided —
	 * restore it between chunks. Tuple length includes the trailing NUL. */
	uint16_t dlen = data->length > 0 ? data->length - 1 : 0;
	if ((uint32_t)s_asm_len + (idx > 0 ? 1 : 0) + dlen + 1 > ASM_BUF_SIZE) {
		APP_LOG(APP_LOG_LEVEL_WARNING, "chunk overflow at %u bytes — truncating",
		        (unsigned)s_asm_len);
	} else {
		if (idx > 0) s_asm_buf[s_asm_len++] = '\n';
		memcpy(s_asm_buf + s_asm_len, data->value->cstring, dlen);
		s_asm_len += dlen;
		s_asm_buf[s_asm_len] = '\0';
	}
	s_asm_next = idx + 1;

	if (more) return;

	dispatch_payload(s_asm_key, s_asm_buf, s_asm_len);
	asm_reset();
}

static void inbox_dropped_handler(AppMessageResult reason, void *context)
{
	APP_LOG(APP_LOG_LEVEL_WARNING, "Inbox drop: %d", reason);
}

static void outbox_failed_handler(DictionaryIterator *iter,
                                  AppMessageResult reason, void *context)
{
	APP_LOG(APP_LOG_LEVEL_WARNING, "Outbox fail: %d (retries left=%d)",
	        reason, s_retries_left);
	if (s_retries_left > 0 && s_pending_park_id[0]) {
		s_retries_left--;
		s_retry_timer = app_timer_register(RETRY_DELAY_MS, retry_tick, NULL);
		return;
	}
	if (s_callback) s_callback(false);
}

static void outbox_sent_handler(DictionaryIterator *iter, void *context)
{
	/* Pebble framework acked send to phone — no action needed. */
}

static void heartbeat_retry_tick(void *ctx);

static void ready_timeout_tick(void *ctx)
{
	s_ready_timer = NULL;
	APP_LOG(APP_LOG_LEVEL_WARNING, "PKJS ready timeout (%dms)", (int)s_heartbeat_interval);
	if (s_callback) s_callback(false);
	s_heartbeat_interval = s_heartbeat_interval * 2;
	if (s_heartbeat_interval > HEARTBEAT_MAX_INTERVAL_MS) {
		s_heartbeat_interval = HEARTBEAT_MAX_INTERVAL_MS;
	}
	s_heartbeat_timer = app_timer_register(s_heartbeat_interval, heartbeat_retry_tick, NULL);
}

static void heartbeat_retry_tick(void *ctx)
{
	s_heartbeat_timer = NULL;
	APP_LOG(APP_LOG_LEVEL_INFO, "heartbeat retry — reopening AppMessage");
	app_message_deregister_callbacks();
	app_message_register_inbox_received(inbox_received_handler);
	app_message_register_inbox_dropped(inbox_dropped_handler);
	app_message_register_outbox_failed(outbox_failed_handler);
	app_message_register_outbox_sent(outbox_sent_handler);
	app_message_open(INBOX_SIZE, OUTBOX_SIZE);
	s_ready_timer = app_timer_register(READY_TIMEOUT_MS, ready_timeout_tick, NULL);
}

void api_init(void)
{
	app_message_register_inbox_received(inbox_received_handler);
	app_message_register_inbox_dropped(inbox_dropped_handler);
	app_message_register_outbox_failed(outbox_failed_handler);
	app_message_register_outbox_sent(outbox_sent_handler);

	AppMessageResult r = app_message_open(INBOX_SIZE, OUTBOX_SIZE);
	if (r != APP_MSG_OK) {
		APP_LOG(APP_LOG_LEVEL_ERROR, "app_message_open failed: %d", r);
	}

	s_ready_timer = app_timer_register(READY_TIMEOUT_MS, ready_timeout_tick, NULL);
}

void api_deinit(void)
{
	if (s_ready_timer) { app_timer_cancel(s_ready_timer); s_ready_timer = NULL; }
	if (s_heartbeat_timer) { app_timer_cancel(s_heartbeat_timer); s_heartbeat_timer = NULL; }
#ifdef MULTI_DESTINATION
	if (s_dest_list_retry_timer) { app_timer_cancel(s_dest_list_retry_timer); s_dest_list_retry_timer = NULL; }
#endif
	asm_reset();
	app_message_deregister_callbacks();
}

bool api_is_connected(void)
{
	return s_pkjs_ready;
}

void api_set_callback(api_park_cb_t cb)
{
	s_callback = cb;
}

/* Internal: send the request currently in s_pending_park_id. */
static void send_request(void)
{
	if (!s_pending_park_id[0]) return;

	DictionaryIterator *iter;
	AppMessageResult r = app_message_outbox_begin(&iter);
	if (r != APP_MSG_OK) {
		APP_LOG(APP_LOG_LEVEL_WARNING, "outbox_begin: %d", r);
		if (s_callback) s_callback(false);
		return;
	}
	dict_write_cstring(iter, MESSAGE_KEY_REQUEST_PARK_ID, s_pending_park_id);
	if (s_pending_force) {
		dict_write_uint8(iter, MESSAGE_KEY_REQUEST_FORCE, 1);
	}
	r = app_message_outbox_send();
	if (r != APP_MSG_OK) {
		APP_LOG(APP_LOG_LEVEL_WARNING, "outbox_send: %d", r);
		if (s_callback) s_callback(false);
	}
}

static AppTimer *s_hours_retry_timer = NULL;

static void hours_retry_tick(void *ctx)
{
	s_hours_retry_timer = NULL;
	api_fetch_all_hours();
}

void api_fetch_all_hours(void)
{
	if (!s_pkjs_ready) {
		/* PKJS not up yet — queue the request so the ready handshake
		 * picks it up automatically. */
		s_all_hours_queued = true;
		return;
	}
	DictionaryIterator *iter;
	AppMessageResult r = app_message_outbox_begin(&iter);
	if (r != APP_MSG_OK) {
		/* Almost always APP_MSG_BUSY (64) when the park-data request
		 * is still in flight at startup. Retry in a couple of seconds
		 * once the outbox frees up. */
		APP_LOG(APP_LOG_LEVEL_INFO, "hours outbox busy (%d) — retrying", r);
		if (!s_hours_retry_timer) {
			s_hours_retry_timer = app_timer_register(2000, hours_retry_tick, NULL);
		}
		return;
	}
	dict_write_uint8(iter, MESSAGE_KEY_REQUEST_ALL_HOURS, 1);
	r = app_message_outbox_send();
	if (r != APP_MSG_OK) {
		APP_LOG(APP_LOG_LEVEL_WARNING, "hours outbox_send: %d", r);
	}
}

static void fetch_park_impl(const char *park_id, bool force)
{
	if (!park_id) return;
	if (s_retry_timer) {
		app_timer_cancel(s_retry_timer);
		s_retry_timer = NULL;
	}
	strncpy(s_pending_park_id, park_id, sizeof(s_pending_park_id) - 1);
	s_pending_park_id[sizeof(s_pending_park_id) - 1] = '\0';
	s_pending_force = force;
	s_retries_left = MAX_RETRIES;

	if (!s_pkjs_ready) {
		s_fetch_queued = true;
		return;
	}
	send_request();
}

void api_fetch_park(const char *park_id)
{
	fetch_park_impl(park_id, false);
}

void api_fetch_park_force(const char *park_id)
{
	fetch_park_impl(park_id, true);
}

#ifdef MULTI_DESTINATION
void api_set_dest_list_callback(api_dest_list_cb_t cb)
{
	s_dest_list_cb = cb;
}

void api_set_dest_parks_callback(api_dest_parks_cb_t cb)
{
	s_dest_parks_cb = cb;
}

static void dest_list_retry_tick(void *ctx)
{
	s_dest_list_retry_timer = NULL;
	api_request_dest_list();
}

/* Re-request cadence when the reply hasn't arrived. The request is a
 * single tiny message so this is cheap; it stops as soon as a
 * DEST_LIST_DATA payload is dispatched (see dispatch_payload). Without
 * it, one lost request or one failed chunk transfer — easy on a busy
 * real-world BT link, invisible on the emulator's perfect one — leaves
 * the picker on "Loading destinations..." with no recovery. */
#define DEST_LIST_WATCHDOG_MS 10000

void api_request_dest_list(void)
{
	if (!s_pkjs_ready) {
		s_dest_list_queued = true;
		return;
	}
	DictionaryIterator *iter;
	AppMessageResult r = app_message_outbox_begin(&iter);
	if (r != APP_MSG_OK) {
		/* Usually APP_MSG_BUSY — a park-data or all-hours request holds
		 * the outbox. Retry shortly. */
		APP_LOG(APP_LOG_LEVEL_INFO, "dest list outbox busy (%d) — retrying", r);
		if (s_dest_list_retry_timer) app_timer_cancel(s_dest_list_retry_timer);
		s_dest_list_retry_timer = app_timer_register(2000, dest_list_retry_tick, NULL);
		return;
	}
	dict_write_uint8(iter, MESSAGE_KEY_DEST_LIST_DATA, 1);
	app_message_outbox_send();
	/* Watchdog: if no dest list lands before this fires, ask again. */
	if (s_dest_list_retry_timer) app_timer_cancel(s_dest_list_retry_timer);
	s_dest_list_retry_timer = app_timer_register(DEST_LIST_WATCHDOG_MS,
	                                             dest_list_retry_tick, NULL);
}

void api_select_destination(const char *dest_id)
{
	if (!dest_id) return;
	if (!s_pkjs_ready) {
		strncpy(s_pending_dest_id, dest_id, sizeof(s_pending_dest_id) - 1);
		s_pending_dest_id[sizeof(s_pending_dest_id) - 1] = '\0';
		return;
	}
	DictionaryIterator *iter;
	AppMessageResult r = app_message_outbox_begin(&iter);
	if (r != APP_MSG_OK) {
		APP_LOG(APP_LOG_LEVEL_WARNING, "dest select outbox_begin: %d", r);
		return;
	}
	dict_write_cstring(iter, MESSAGE_KEY_DEST_SELECT, dest_id);
	app_message_outbox_send();
}
#endif
