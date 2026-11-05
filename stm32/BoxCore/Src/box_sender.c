/* Event sender: see box_sender.h. */
#include "box_sender.h"

#include <string.h>

void box_sender_init(BoxSender *sender, BoxSenderOutputs outputs)
{
    memset(sender, 0, sizeof *sender);
    sender->outputs = outputs;
    sender->next_seq = 1;
}

static void send_oldest(BoxSender *sender, uint32_t now_ms)
{
    char line[BOX_LINE_MAX];
    box_lines_format_event(line, sender->outbox[0].seq, sender->outbox[0].event);
    sender->sent_at_ms = now_ms;
    if (sender->outputs.write_line != NULL) {
        sender->outputs.write_line(sender->outputs.context, line);
    }
}

/* Dropping makes room behind the Event in flight, so there must be a spot
 * behind it. (C99 has no static_assert; a negative array size fails.) */
typedef char box_sender_capacity_is_at_least_2[(BOX_SENDER_CAPACITY >= 2u) ? 1 : -1];

/* Drop outbox[1], the oldest Event waiting behind the one in flight. */
static void drop_oldest_waiting(BoxSender *sender)
{
    BoxSenderEntry dropped = sender->outbox[1];
    sender->count--;
    memmove(&sender->outbox[1], &sender->outbox[2],
            (sender->count - 1u) * sizeof sender->outbox[0]);
    if (sender->outputs.on_dropped != NULL) {
        sender->outputs.on_dropped(sender->outputs.context, dropped.seq, dropped.event);
    }
}

uint32_t box_sender_add(BoxSender *sender, BoxEvent event, uint32_t now_ms)
{
    uint32_t seq = sender->next_seq++;
    if (sender->count == BOX_SENDER_CAPACITY) {
        drop_oldest_waiting(sender);
    }
    sender->outbox[sender->count].seq = seq;
    sender->outbox[sender->count].event = event;
    sender->count++;
    if (sender->count == 1u) {
        send_oldest(sender, now_ms);
    }
    return seq;
}

void box_sender_ack(BoxSender *sender, uint32_t seq, uint32_t now_ms)
{
    if (sender->count == 0u || seq != sender->outbox[0].seq) {
        return;   /* not the Event in flight: late, duplicate or noise */
    }
    sender->count--;
    memmove(&sender->outbox[0], &sender->outbox[1],
            sender->count * sizeof sender->outbox[0]);
    if (sender->count > 0u) {
        send_oldest(sender, now_ms);
    }
}

void box_sender_tick(BoxSender *sender, uint32_t now_ms)
{
    /* The age as a signed difference: right across the wraparound, and
     * negative (not due) if now_ms is a little older than the send, as when
     * an ACK stamped later in the same main-loop pass sent this Event. */
    if (sender->count > 0u &&
        (int32_t)(now_ms - sender->sent_at_ms) >= (int32_t)BOX_SENDER_RESEND_MS) {
        send_oldest(sender, now_ms);
    }
}
