/*
 * The time stamp on an Event: when it arrived at the ESP32 from the STM32,
 * or "unknown" if the ESP32's clock has never been set by NTP. It must
 * never show as 1970 (product.md section 5, "Cloud").
 *
 * Pure C99, tested on the PC (`make test`). main.cpp reads the clock with
 * time(nullptr). After a power-on reset that counts from 0 until NTP
 * syncs, so any reading before EVENT_TIME_SANE_MIN counts as never synced.
 * A soft reset (a panic, the watchdog, esp_restart()) keeps the RTC's
 * time, so Events after one get a near-real time even before NTP syncs
 * again (it may have drifted a little).
 */
#ifndef EVENT_TIME_H
#define EVENT_TIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 2024-01-01T00:00:00Z. No Event can really happen before this, and a
 * clock counting from boot won't reach it (that would take 54 years). */
#define EVENT_TIME_SANE_MIN 1704067200LL

/* The last second written with a 4-digit year: 9999-12-31T23:59:59Z. A
 * reading past it is garbage, so it counts as unknown too. */
#define EVENT_TIME_SANE_MAX 253402300799LL

/* Room for "2026-11-08T14:32:05Z" and its NUL. */
#define EVENT_TIME_TEXT_MAX 21

typedef struct {
    bool known;       /* false: NTP had never synced ("unknown") */
    int64_t unix_s;   /* seconds since 1970-01-01T00:00:00Z; 0 if unknown */
} EventTime;

/* A reading of the clock, in seconds since 1970 (a time_t). */
EventTime event_time_from_clock(int64_t clock_s);

/* "2026-11-08T14:32:05Z" (UTC, ISO 8601), or "unknown". */
void event_time_format(EventTime time, char text[EVENT_TIME_TEXT_MAX]);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_TIME_H */
