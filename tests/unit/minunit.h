/**
 * minunit.h - Minimal unit testing framework
 *
 * Simple assertion-based testing with minimal overhead.
 * Based on: http://www.jera.com/techinfo/jtns/jtn002.html
 */

#ifndef MINUNIT_H
#define MINUNIT_H

#include <stdio.h>
#include <string.h>

/* Global test counters */
extern int tests_run;
extern int tests_failed;

/* Assertion macros */
#define mu_assert(message, test)                                                    \
    do                                                                              \
    {                                                                               \
        if (!(test))                                                                \
        {                                                                           \
            fprintf(stderr, "FAIL: %s\n  at %s:%d\n", message, __FILE__, __LINE__); \
            tests_failed++;                                                         \
            return message;                                                         \
        }                                                                           \
    } while (0)

#define mu_assert_eq(message, expected, actual)                                                       \
    do                                                                                                \
    {                                                                                                 \
        if ((expected) != (actual))                                                                   \
        {                                                                                             \
            fprintf(stderr, "FAIL: %s\n  Expected: %lu\n  Actual: %lu\n  at %s:%d\n",                 \
                    message, (unsigned long)(expected), (unsigned long)(actual), __FILE__, __LINE__); \
            tests_failed++;                                                                           \
            return message;                                                                           \
        }                                                                                             \
    } while (0)

#define mu_assert_str_eq(message, expected, actual)                                         \
    do                                                                                      \
    {                                                                                       \
        if (strcmp((expected), (actual)) != 0)                                              \
        {                                                                                   \
            fprintf(stderr, "FAIL: %s\n  Expected: \"%s\"\n  Actual: \"%s\"\n  at %s:%d\n", \
                    message, (expected), (actual), __FILE__, __LINE__);                     \
            tests_failed++;                                                                 \
            return message;                                                                 \
        }                                                                                   \
    } while (0)

/* Test runner macros */
#define mu_run_test(test)             \
    do                                \
    {                                 \
        const char *message = test(); \
        tests_run++;                  \
        if (message)                  \
            return message;           \
    } while (0)

#define mu_report()                                             \
    do                                                          \
    {                                                           \
        printf("\n=== Test Summary ===\n");                     \
        printf("Tests run: %d\n", tests_run);                   \
        printf("Tests passed: %d\n", tests_run - tests_failed); \
        printf("Tests failed: %d\n", tests_failed);             \
        if (tests_failed == 0)                                  \
        {                                                       \
            printf("\nALL TESTS PASSED\n");                     \
        }                                                       \
        else                                                    \
        {                                                       \
            printf("\nSOME TESTS FAILED\n");                    \
        }                                                       \
        return tests_failed;                                    \
    } while (0)

#endif /* MINUNIT_H */
