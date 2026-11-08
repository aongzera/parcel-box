/*
 * The ESP32's offline Event queue: holds each Event the STM32 sends, with
 * the time it arrived, until it has been uploaded to the cloud, so Events
 * that happen while WiFi or Firebase is down reach the Owner in order and
 * with their real times once the connection is back (product.md section 4,
 * "Without WiFi", and section 5).
 *
 * Pure C99 with no Arduino calls, so it is tested on the PC (`make test`).
 * main.cpp supplies the clock and the upload.
 *
 * How to use it:
 *   1. event_queue_init() with your outputs.
 *   2. event_queue_push() each Event from bridge_link's on_event, which is
 *      called the moment its line arrives: the Event is stamped then, with
 *      clock_s() (see event_time.h: before NTP syncs it is "unknown").
 *   3. event_queue_tick() from the main loop, as often as you like.
 *
 * Uploading: each tick tries at most one upload, of the oldest Event, so
 * the main loop keeps reading the UART between uploads. If upload()
 * returns true, the Event leaves the queue and the next tick uploads the
 * next one. If it returns false, the Event stays at the head and no upload
 * is tried again until EVENT_QUEUE_RETRY_MS later, so a down link isn't
 * hammered, unless event_queue_retry_now() ends the wait (main.cpp calls
 * it when WiFi connects). Events are never reordered or skipped.
 *
 * Queue full: pushing onto a full queue drops the oldest Event and reports
 * it to on_dropped (main.cpp prints `# Queue full: dropped Event <seq>`).
 * The newest Events are kept, like the STM32's outbox. The queue is in RAM
 * only, so it doesn't survive a power loss (product.md section 10).
 *
 * ACKs don't wait for the queue: bridge_link sends `ACK,<seq>` before it
 * calls on_event, and the queue never talks to the STM32, so the STM32's
 * outbox keeps moving while the ESP32 is offline.
 *
 * Note for #23 (the real Firebase write): upload() runs inside the main
 * loop, so while it runs no UART bytes are read (the UART driver buffers
 * them). Keep each write short, with a timeout well under a second, or
 * make it asynchronous. If it becomes asynchronous, the Event being
 * uploaded must not be dropped by a full queue, and only one may be in
 * flight at a time, to keep the order.
 *
 * Also for #23, duplicates: a write that is stored but times out (its
 * reply is lost) returns false, so the same Event is uploaded again and
 * stored twice. To avoid that, give each queued Event a key made once,
 * before its first try (e.g. a Firebase push key generated on the ESP32),
 * and write to that key, so a retry overwrites instead of adding. Or
 * accept rare duplicates in the Event log. The seq is not a unique key:
 * it restarts at 1 after every BOOT (each STM32 reset).
 */
#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "bridge_link.h"
#include "event_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How many Events the queue holds. A Lid cycle takes several seconds, so
 * this covers well over the few minutes a WiFi outage lasts in practice
 * (even the demo's worst case, the room's WiFi failing), at 24 bytes an
 * Event (~1.5 KB of the ESP32-S3's 512 KB RAM). */
#define EVENT_QUEUE_CAPACITY 64u

/* How long to wait after a failed upload before trying again. */
#define EVENT_QUEUE_RETRY_MS 5000u

/* An Event and the time it arrived from the STM32. */
typedef struct {
    BridgeEvent event;
    EventTime time;
} QueuedEvent;

typedef struct {
    void *context;
    /* The clock, in seconds since 1970 (a time_t). May be NULL: every
     * Event's time is then "unknown". */
    int64_t (*clock_s)(void *context);
    /* Upload one Event; true if it was stored in the cloud. */
    bool (*upload)(void *context, const QueuedEvent *queued);
    /* An Event was dropped because the queue was full. May be NULL. */
    void (*on_dropped)(void *context, const QueuedEvent *dropped);
} EventQueueOutputs;

/* Declared here so it can live in a static variable; treat the fields as
 * private. */
typedef struct {
    EventQueueOutputs outputs;
    QueuedEvent entries[EVENT_QUEUE_CAPACITY];   /* a ring, oldest at head */
    uint32_t head;
    uint32_t count;
    bool backing_off;        /* the last upload failed */
    uint32_t failed_at_ms;   /* when it failed */
} EventQueue;

void event_queue_init(EventQueue *queue, EventQueueOutputs outputs);

/* An Event arrived: stamp it with the clock now and queue it. */
void event_queue_push(EventQueue *queue, BridgeEvent event);

/* Try to upload the oldest Event, if there is one and no retry wait is
 * running. `now_ms` is a millisecond clock that may wrap around. */
void event_queue_tick(EventQueue *queue, uint32_t now_ms);

/* The connection is back (main.cpp: WiFi just connected): end any retry
 * wait, so the next tick tries the oldest Event at once. Harmless if no
 * wait is running. */
void event_queue_retry_now(EventQueue *queue);

/* How many Events are waiting to be uploaded. */
uint32_t event_queue_count(const EventQueue *queue);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_QUEUE_H */
