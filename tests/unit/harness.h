/* harness.h - minimal TAP-compatible test harness for libzenctl
 *
 * No external deps. Each test file includes this header and uses the
 * OK / FAIL macros. Counters are file-static so each translation unit
 * has its own; suite entry points reset them with SUITE_START and
 * report with SUITE_END.
 */
#ifndef ZENCTL_TEST_HARNESS_H
#define ZENCTL_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* File-local counters. Each .c that includes this gets its own copy. */
static int test_count = 0;
static int test_pass = 0;
static int test_fail = 0;

#define OK(cond, name) do { \
    test_count++; \
    if (cond) { test_pass++; printf("ok %d: %s\n", test_count, name); } \
    else { test_fail++; printf("not ok %d: %s\n", test_count, name); } \
} while(0)

#define FAIL(name) do { \
    test_count++; test_fail++; \
    printf("not ok %d: %s\n", test_count, name); \
} while(0)

/* Reset counters at the start of a suite and print a banner. */
#define SUITE_START(name) do { \
    test_count = 0; test_pass = 0; test_fail = 0; \
    printf("\n=== %s ===\n", name); \
} while(0)

/* Print suite summary line. */
#define SUITE_END() do { \
    printf("--- suite: %d test(s), %d pass, %d fail\n", \
           test_count, test_pass, test_fail); \
} while(0)

/* Number of failures in this suite (for aggregation by test_main). */
#define SUITE_FAILURES() (test_fail)

/* For a single-suite binary: print summary and return the right exit code. */
#define TEST_DONE() do { \
    printf("\n%d test(s), %d pass, %d fail\n", test_count, test_pass, test_fail); \
    return test_fail > 0 ? 1 : 0; \
} while(0)

#endif /* ZENCTL_TEST_HARNESS_H */
