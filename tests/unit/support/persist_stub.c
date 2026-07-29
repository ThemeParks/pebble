/*
 * persist_stub.c — in-memory implementation of the Pebble persist API.
 *
 * A fixed table of key→blob entries. Fixed sizes rather than malloc so
 * a test that leaks keys fails loudly (write returns an error) instead
 * of quietly growing. Blob capacity comfortably exceeds the largest
 * thing storage.c writes (a favourites array: 8 * 12 = 96 bytes).
 */
#include <pebble.h>
#include "persist_stub.h"
#include <string.h>

#define STUB_MAX_KEYS 64
#define STUB_MAX_BLOB 256

typedef struct {
	uint32_t key;
	bool used;
	size_t len;
	uint8_t data[STUB_MAX_BLOB];
} stub_entry_t;

static stub_entry_t s_entries[STUB_MAX_KEYS];
static int s_read_count;
static int s_write_count;

void persist_stub_reset(void)
{
	memset(s_entries, 0, sizeof(s_entries));
	s_read_count = 0;
	s_write_count = 0;
}

int persist_stub_read_count(void)
{
	return s_read_count;
}

int persist_stub_write_count(void)
{
	return s_write_count;
}

static stub_entry_t *find(uint32_t key)
{
	for (int i = 0; i < STUB_MAX_KEYS; i++) {
		if (s_entries[i].used && s_entries[i].key == key) return &s_entries[i];
	}
	return NULL;
}

static stub_entry_t *find_or_alloc(uint32_t key)
{
	stub_entry_t *e = find(key);
	if (e) return e;
	for (int i = 0; i < STUB_MAX_KEYS; i++) {
		if (!s_entries[i].used) {
			s_entries[i].used = true;
			s_entries[i].key = key;
			s_entries[i].len = 0;
			return &s_entries[i];
		}
	}
	return NULL;
}

bool persist_exists(const uint32_t key)
{
	return find(key) != NULL;
}

int32_t persist_read_int(const uint32_t key)
{
	s_read_count++;
	stub_entry_t *e = find(key);
	if (!e || e->len < sizeof(int32_t)) return 0;
	int32_t v;
	memcpy(&v, e->data, sizeof(v));
	return v;
}

status_t persist_write_int(const uint32_t key, const int32_t value)
{
	s_write_count++;
	stub_entry_t *e = find_or_alloc(key);
	if (!e) return -1;
	memcpy(e->data, &value, sizeof(value));
	e->len = sizeof(value);
	return S_SUCCESS;
}

/* Strings are stored including their terminating NUL (matching what the
 * SDK does under the hood). On a short read the last copied byte is
 * forced to NUL so callers always get a valid C string. */
int persist_read_string(const uint32_t key, char *buffer, const size_t buffer_size)
{
	s_read_count++;
	stub_entry_t *e = find(key);
	if (!e || buffer_size == 0) return E_DOES_NOT_EXIST;
	size_t n = e->len;
	if (n > buffer_size) n = buffer_size;
	memcpy(buffer, e->data, n);
	if (n > 0) buffer[n - 1] = '\0';
	return (int)n;
}

status_t persist_write_string(const uint32_t key, const char *cstring)
{
	s_write_count++;
	stub_entry_t *e = find_or_alloc(key);
	if (!e || !cstring) return -1;
	size_t n = strlen(cstring) + 1;
	if (n > STUB_MAX_BLOB) n = STUB_MAX_BLOB;
	memcpy(e->data, cstring, n);
	e->data[n - 1] = '\0';
	e->len = n;
	return S_SUCCESS;
}

int persist_read_data(const uint32_t key, void *buffer, const size_t buffer_size)
{
	s_read_count++;
	stub_entry_t *e = find(key);
	if (!e) return E_DOES_NOT_EXIST;
	size_t n = e->len;
	if (n > buffer_size) n = buffer_size;
	memcpy(buffer, e->data, n);
	return (int)n;
}

status_t persist_write_data(const uint32_t key, const void *data, const size_t size)
{
	s_write_count++;
	stub_entry_t *e = find_or_alloc(key);
	if (!e || size > STUB_MAX_BLOB) return -1;
	memcpy(e->data, data, size);
	e->len = size;
	return S_SUCCESS;
}

status_t persist_delete(const uint32_t key)
{
	stub_entry_t *e = find(key);
	if (!e) return E_DOES_NOT_EXIST;
	e->used = false;
	e->len = 0;
	return S_SUCCESS;
}
