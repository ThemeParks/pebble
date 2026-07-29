/*
 * persist_stub.h — test-only controls for the in-memory persist store.
 *
 * The persist_* functions themselves are declared in the stub pebble.h
 * (matching the SDK); this header adds the instrumentation the tests
 * use: a reset between test cases, and read/write call counters so
 * later performance tests can assert on persist traffic.
 */
#pragma once

/* Wipe all stored keys and zero the call counters. Call at the start of
 * every test case — the store is process-global, like the real thing. */
void persist_stub_reset(void);

/* Number of persist_read_* calls since the last reset. persist_exists
 * is not counted — it doesn't touch flash meaningfully on hardware. */
int persist_stub_read_count(void);

/* Number of persist_write_* calls since the last reset. */
int persist_stub_write_count(void);
