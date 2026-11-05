/* Event sender: see box_sender.h. */
#include "box_sender.h"

#include <string.h>

void box_sender_init(BoxSender *sender, BoxSenderOutputs outputs)
{
    memset(sender, 0, sizeof *sender);
    sender->outputs = outputs;
    sender->next_seq = 1;
}

static void write_line(BoxSender *sender, const char *line, uint32_t now_ms)
{
    sender->sent_at_ms = now_ms;
    if (sender->outputs.write_line != NULL) {
        sender->outputs.write_line(sender->outputs.context, line);
    }
}

static void send_oldest(BoxSender *sender, uint32_t now_ms)
{
    char line[BOX_LINE_MAX];
    box_lines_format_event(line, sender->outbox[0].seq, sender->outbox[0].event);
    write_line(sender, line, now_ms);
}

void box_sender_start(BoxSender *sender, uint32_t now_ms)
{
    if (sender->session == BOX_SESSION_NOT_STARTED) {
        sender->session = BOX_SESSION_BOOTING;
        write_line(sender, BOX_BOOT_LINE, now_ms);
    }
}

/* Once the session is open, dropping makes room behind the Event in flight,
 * so there must be a spot behind it. (C99 has no static_assert; a negative
 * array size fails.) */
typedef char box_sender_capacity_is_at_least_2[(BOX_SENDER_CAPACITY >= 2u) ? 1 : -1];

/* Drop the oldest Event that is only waiting: outbox[1] once the session is
 * open (outbox[0] is in flight), or outbox[0] before then (nothing has been
 * sent yet). */
static void drop_oldest_waiting(BoxSender *sender)
{
    uint32_t first = (sender->session == BOX_SESSION_OPEN) ? 1u : 0u;
    BoxSenderEntry dropped = sender->outbox[first];
    sender->count--;
    memmove(&sender->outbox[first], &sender->outbox[first + 1u],
            (sender->count - first) * sizeof sender->outbox[0]);
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
    if (sender->session == BOX_SESSION_OPEN && sender->count == 1u) {
        send_oldest(sender, now_ms);
    }
    return seq;
}

bool box_sender_ack(BoxSender *sender, uint32_t seq, uint32_t now_ms)
{
    if (sender->session == BOX_SESSION_BOOTING && seq == BOX_BOOT_SEQ) {
        sender->session = BOX_SESSION_OPEN;
        if (sender->count > 0u) {
            send_oldest(sender, now_ms);
        }
        return true;
    }
    if (sender->session != BOX_SESSION_OPEN || sender->count == 0u || seq != sender->outbox[0].seq) {
        return false;   /* not the Event in flight: late, duplicate or noise */
    }
    sender->count--;
    memmove(&sender->outbox[0], &sender->outbox[1],
            sender->count * sizeof sender->outbox[0]);
    if (sender->count > 0u) {
        send_oldest(sender, now_ms);
    }
    return true;
}

void box_sender_tick(BoxSender *sender, uint32_t now_ms)
{
    /* The age as a signed difference: right across the wraparound, and
     * negative (not due) if now_ms is a little older than the send, as when
     * an ACK stamped later in the same main-loop pass sent this Event. */
    if ((int32_t)(now_ms - sender->sent_at_ms) < (int32_t)BOX_SENDER_RESEND_MS) {
        return;
    }
    if (sender->session == BOX_SESSION_BOOTING) {
        write_line(sender, BOX_BOOT_LINE, now_ms);
    } else if (sender->session == BOX_SESSION_OPEN && sender->count > 0u) {
        send_oldest(sender, now_ms);
    }
}
