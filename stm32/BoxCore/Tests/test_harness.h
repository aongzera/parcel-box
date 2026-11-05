/* A tiny assert harness for the PC tests. No external dependencies.
 * The counters are defined once in test_harness.c, so every test file
 * shares them and no failure is lost. */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>

extern int test_failures;
extern int test_current_failed;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("    %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            test_current_failed = 1;                                      \
        }                                                                 \
    } while (0)

#define CHECK_EQ_INT(expected, actual)                                    \
    do {                                                                  \
        long e_ = (long)(expected), a_ = (long)(actual);                  \
        if (e_ != a_) {                                                   \
            printf("    %s:%d: expected %s == %ld, got %ld\n",            \
                   __FILE__, __LINE__, #actual, e_, a_);                  \
            test_current_failed = 1;                                      \
        }                                                                 \
    } while (0)

#define RUN_TEST(fn)                                                      \
    do {                                                                  \
        test_current_failed = 0;                                          \
        fn();                                                             \
        printf("%s %s\n", test_current_failed ? "FAIL" : "ok  ", #fn);   \
        test_failures += test_current_failed;                             \
    } while (0)

#define TEST_SUMMARY()                                                    \
    (printf("\n%s: %d test(s) failed\n",                                  \
            test_failures ? "FAILED" : "PASSED", test_failures),          \
     test_failures ? 1 : 0)

#endif /* TEST_HARNESS_H */
