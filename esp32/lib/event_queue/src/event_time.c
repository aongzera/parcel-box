/* The time stamp on an Event: see event_time.h. */
#include "event_time.h"

#include <stdio.h>
#include <string.h>

EventTime event_time_from_clock(int64_t clock_s)
{
    EventTime time = { false, 0 };
    if (clock_s >= EVENT_TIME_SANE_MIN && clock_s <= EVENT_TIME_SANE_MAX) {
        time.known = true;
        time.unix_s = clock_s;
    }
    return time;
}

void event_time_format(EventTime time, char text[EVENT_TIME_TEXT_MAX])
{
    int64_t days, seconds, z, era, doe, yoe, doy, mp, year, month, day;

    if (!time.known || time.unix_s < 0 || time.unix_s > EVENT_TIME_SANE_MAX) {
        strcpy(text, "unknown");
        return;
    }
    days = time.unix_s / 86400;
    seconds = time.unix_s % 86400;

    /* Days since 1970-01-01 to a date in the Gregorian calendar (Howard
     * Hinnant's civil_from_days), with no time zone or C library clock. */
    z = days + 719468;   /* days since 0000-03-01 */
    era = z / 146097;    /* 400-year eras */
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;   /* months since March */
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = yoe + era * 400 + (month <= 2 ? 1 : 0);

    snprintf(text, EVENT_TIME_TEXT_MAX, "%04u-%02u-%02uT%02u:%02u:%02uZ",
             (unsigned)year, (unsigned)month, (unsigned)day,
             (unsigned)(seconds / 3600), (unsigned)(seconds / 60 % 60),
             (unsigned)(seconds % 60));
}
