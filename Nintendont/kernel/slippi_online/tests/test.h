/* Minimal test macros for the host unit tests. */
#ifndef SO_TEST_H
#define SO_TEST_H

#include <stdio.h>
#include <string.h>

extern int test_failures;
extern int test_checks;

#define CHECK(cond) do { \
	test_checks++; \
	if (!(cond)) { \
		printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
		test_failures++; \
	} \
} while (0)

#define CHECK_INT(actual, expected) do { \
	long long a_ = (long long)(actual), e_ = (long long)(expected); \
	test_checks++; \
	if (a_ != e_) { \
		printf("  %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #actual, a_, e_); \
		test_failures++; \
	} \
} while (0)

#define CHECK_STR(actual, expected) do { \
	const char *a_ = (actual), *e_ = (expected); \
	test_checks++; \
	if (strcmp(a_, e_) != 0) { \
		printf("  %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #actual, a_, e_); \
		test_failures++; \
	} \
} while (0)

#define RUN(fn) do { printf("%s\n", #fn); fn(); } while (0)

#endif
