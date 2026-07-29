/*
 * pebble.h — host-side stub of the Pebble SDK header.
 *
 * The unit-test runner puts tests/unit/support ahead of src/c on the
 * include path, so `#include <pebble.h>` in the pure-logic sources
 * (parkdata.c, sort.c, storage.c) resolves here instead of the real SDK.
 * Only the slice of the SDK those files actually use is stubbed — if a
 * new SDK call shows up in a logic file, add it here rather than pulling
 * UI files into the host build.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>   /* the real pebble.h also provides snprintf & co. */

/* APP_LOG is a no-op on the host. Level names kept so call sites
 * compile unchanged; values match the SDK's AppLogLevel enum. */
#define APP_LOG_LEVEL_ERROR 1
#define APP_LOG_LEVEL_WARNING 50
#define APP_LOG_LEVEL_INFO 100
#define APP_LOG_LEVEL_DEBUG 200
#define APP_LOG_LEVEL_DEBUG_VERBOSE 255
#define APP_LOG(...) ((void)0)

/* Persist API subset used by storage.c. Signatures mirror the real SDK
 * (status_t is int32_t there too); the in-memory implementation lives in
 * persist_stub.c. */
typedef int32_t status_t;
#define S_SUCCESS 0
#define E_DOES_NOT_EXIST -7

bool persist_exists(const uint32_t key);
int32_t persist_read_int(const uint32_t key);
status_t persist_write_int(const uint32_t key, const int32_t value);
int persist_read_string(const uint32_t key, char *buffer, const size_t buffer_size);
status_t persist_write_string(const uint32_t key, const char *cstring);
int persist_read_data(const uint32_t key, void *buffer, const size_t buffer_size);
status_t persist_write_data(const uint32_t key, const void *data, const size_t size);
status_t persist_delete(const uint32_t key);
