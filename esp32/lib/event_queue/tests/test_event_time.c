/*
 * PC tests for the Event time stamp (event_time): a reading of the ESP32's
 * clock becomes a time, or "unknown" if NTP has never synced, and is written
 * as text for the debug log. Never 1970 (product.md section 5, "Cloud").
 */
#include <string.h>

#include "event_time.h"
#include "test_harness.h"

#define CHECK_TEXT(expected, time)                                          \
    do {                                                                    \
        char text_[EVENT_TIME_TEXT_MAX];                                    \
        event_time_format((time), text_);                                   \
        if (strcmp(text_, (expected)) != 0) {                               \
            printf("    %s:%d: expected \"%s\", got \"%s\"\n",              \
                   __FILE__, __LINE__, (expected), text_);                  \
            test_current_failed = 1;                                        \
        }                                                                   \
    } while (0)

static void test_a_clock_that_has_synced_gives_that_time(void)
{
    EventTime time = event_time_from_clock(1794148325);
    CHECK(time.known);
    CHECK(time.unix_s == 1794148325);
    CHECK_TEXT("2026-11-08T14:32:05Z", time);
}

static void test_a_clock_that_never_synced_gives_unknown_not_1970(void)
{
    /* Before NTP syncs, the clock counts from 0 at boot. */
    static const long long unsynced[] = { 0, 1, 2, 86400, 1704067199 };
    size_t i;

    for (i = 0; i < sizeof unsynced / sizeof unsynced[0]; i++) {
        EventTime time = event_time_from_clock(unsynced[i]);
        CHECK(!time.known);
        CHECK_TEXT("unknown", time);
    }
}

static void test_a_negative_clock_gives_unknown(void)
{
    CHECK(!event_time_from_clock(-1).known);
}

static void test_the_first_sane_second_is_known(void)
{
    EventTime time = event_time_from_clock(1704067200);
    CHECK(time.known);
    CHECK_TEXT("2024-01-01T00:00:00Z", time);
}

static void test_times_are_written_as_utc_dates_across_leap_days(void)
{
    CHECK_TEXT("2024-02-29T12:34:56Z", event_time_from_clock(1709210096));
    CHECK_TEXT("2100-03-01T00:00:00Z", event_time_from_clock(4107542400));
}

static void test_the_last_sane_second_is_known_and_the_next_is_unknown(void)
{
    EventTime last = event_time_from_clock(253402300799LL);
    CHECK(last.known);
    CHECK_TEXT("9999-12-31T23:59:59Z", last);
    CHECK(!event_time_from_clock(253402300800LL).known);
    CHECK_TEXT("unknown", event_time_from_clock(253402300800LL));
}

int main(void)
{
    RUN_TEST(test_a_clock_that_has_synced_gives_that_time);
    RUN_TEST(test_a_clock_that_never_synced_gives_unknown_not_1970);
    RUN_TEST(test_a_negative_clock_gives_unknown);
    RUN_TEST(test_the_first_sane_second_is_known);
    RUN_TEST(test_times_are_written_as_utc_dates_across_leap_days);
    RUN_TEST(test_the_last_sane_second_is_known_and_the_next_is_unknown);
    return TEST_SUMMARY();
}
