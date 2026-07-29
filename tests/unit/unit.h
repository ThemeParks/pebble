/*
 * unit.h — minimal single-file assert framework for the host-side tests.
 *
 * No framework dependency on purpose: each test_*.c is a standalone
 * binary with its own main(), so a plain gcc is the only requirement.
 * Failures print file:line + expected/actual and bump a counter; the
 * binary's exit code (via unit_summary) is what the runner checks.
 */
#pragma once

#include <stdio.h>
#include <string.h>

static int g_unit_failures = 0;

#define RUN_TEST(fn) do { \
	printf("--- %s\n", #fn); \
	fn(); \
} while (0)

#define ASSERT_TRUE(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: ASSERT_TRUE(%s)\n", __FILE__, __LINE__, #cond); \
		g_unit_failures++; \
	} \
} while (0)

#define ASSERT_EQ_INT(expected, actual) do { \
	long long _e = (long long)(expected); \
	long long _a = (long long)(actual); \
	if (_e != _a) { \
		printf("FAIL %s:%d: %s == %s: expected %lld, got %lld\n", \
		       __FILE__, __LINE__, #expected, #actual, _e, _a); \
		g_unit_failures++; \
	} \
} while (0)

#define ASSERT_EQ_STR(expected, actual) do { \
	const char *_e = (expected); \
	const char *_a = (actual); \
	if (_e == NULL || _a == NULL || strcmp(_e, _a) != 0) { \
		printf("FAIL %s:%d: %s: expected \"%s\", got \"%s\"\n", \
		       __FILE__, __LINE__, #actual, \
		       _e ? _e : "(null)", _a ? _a : "(null)"); \
		g_unit_failures++; \
	} \
} while (0)

static int unit_summary(void)
{
	if (g_unit_failures > 0) {
		printf("FAILED: %d assertion(s)\n", g_unit_failures);
		return 1;
	}
	printf("OK\n");
	return 0;
}
