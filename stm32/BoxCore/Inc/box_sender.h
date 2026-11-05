/*
 * Event sender: numbers each Event and sends its EVT line to the ESP32
 * again and again until the ESP32 ACKs it, so no Event is lost when the
 * UART link drops for a moment (product.md section 5).
 *
 * Pure C99 with no HAL, like the core, so it is tested on the PC
 * (`make test`). The hardware adapter supplies `write_line` (the same
 * writer box_lines uses) and feeds in the ACKs that box_rx reads.
 *
 * How to use it:
 *   1. box_sender_init() with your outputs.
 *   2. box_sender_add() each Event the core reports (its on_event).
 *   3. box_sender_ack() each `ACK,<seq>` that box_rx reads (its on_ack).
 *   4. box_sender_tick() as often as box_core_tick() (e.g. every 10 ms).
 *
 * Sequence numbers: each Event gets its number when it is added, starting
 * at 1 at power-up and going up by one per Event, so the numbers follow the
 * order the Events happened in. They restart at 1 after a reboot; telling
 * a new session from a duplicate is the ESP32's job (#29).
 *
 * The outbox holds up to BOX_SENDER_CAPACITY Events, oldest first. Only the
 * oldest is in flight: its EVT line is sent as soon as it becomes the
 * oldest, then again every BOX_SENDER_RESEND_MS until `ACK,<its seq>`
 * arrives. Then it leaves the outbox and the next one is sent at once. So
 * Events leave strictly in order, one at a time. An ACK for any other
 * sequence number (a late or duplicate ACK, or noise) is ignored.
 *
 * Outbox full: adding an Event to a full outbox drops the oldest Event
 * that is waiting behind the one in flight, and reports it to on_dropped.
 * The one in flight is never dropped, as the ESP32 may already have it. The
 * newest Events are kept, as in the ESP32's offline queue, and the numbers
 * sent still only go up (the dropped one leaves a gap). Queued Events don't
 * survive a power loss (product.md section 10).
 */
#ifndef BOX_SENDER_H
#define BOX_SENDER_H

#include <stdint.h>

#include "box_core.h"
#include "box_lines.h"

/* How many Events the outbox holds, counting the one in flight. A Lid
 * cycle takes seconds, so this covers a link outage of a minute or so of
 * busy use; the ESP32 queues for longer outages (WiFi). At least 2. */
#define BOX_SENDER_CAPACITY 8u

/* How long to wait for an ACK before sending the Event again. The ESP32
 * ACKs within milliseconds, so this only matters when a line or its ACK
 * was lost; it keeps the link from flooding while the ESP32 is away. */
#define BOX_SENDER_RESEND_MS 1000u

typedef struct {
    void *context;
    /* Send one protocol line (ending in "\n", NUL-terminated). */
    void (*write_line)(void *context, const char *line);
    /* An Event was dropped because the outbox was full. May be NULL. */
    void (*on_dropped)(void *context, uint32_t seq, BoxEvent event);
} BoxSenderOutputs;

/* Declared here so it can live in a static variable; treat the fields as
 * private. */
typedef struct {
    uint32_t seq;
    BoxEvent event;
} BoxSenderEntry;

typedef struct {
    BoxSenderOutputs outputs;
    BoxSenderEntry outbox[BOX_SENDER_CAPACITY];   /* [0] is in flight */
    uint32_t count;
    uint32_t next_seq;
    uint32_t sent_at_ms;    /* when outbox[0] was last sent */
} BoxSender;

void box_sender_init(BoxSender *sender, BoxSenderOutputs outputs);

/* An Event happened: give it the next sequence number and queue it. Sent at
 * once if the outbox was empty. Returns its sequence number. */
uint32_t box_sender_add(BoxSender *sender, BoxEvent event, uint32_t now_ms);

/* The ESP32 sent `ACK,<seq>`. */
void box_sender_ack(BoxSender *sender, uint32_t seq, uint32_t now_ms);

/* Resend the Event in flight when it is due. `now_ms` may wrap around, and
 * may be a little older than the last add or ack (a time before the send
 * is never due). Pass the same clock to all three. */
void box_sender_tick(BoxSender *sender, uint32_t now_ms);

#endif /* BOX_SENDER_H */
