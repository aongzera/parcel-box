/*
 * PC tests of the UART protocol across both boards: the STM32's line writer
 * (box_lines) feeds the ESP32's link (bridge_link), and the ESP32's replies
 * go back through the STM32's line reader (box_rx). A change to a line on
 * one side only (a renamed outcome, a reordered field) fails here.
 */
#include <string.h>

#include "box_lines.h"
#include "box_rx.h"
#include "box_sender.h"
#include "bridge_link.h"
#include "test_harness.h"

#define MAX_OUT 8

typedef struct {
    BridgeLink link;     /* the ESP32 end */
    BoxRx rx;            /* the STM32 end, reading the ESP32's replies */
    BoxLines lines;      /* the STM32 end, writing STS lines */
    BoxSender sender;    /* the STM32 end, sending Events (start_with_sender) */
    uint32_t now_ms;     /* the STM32's clock, for the sender */
    bool lose_replies;   /* the ESP32's replies never reach the STM32 */
    bool lose_lines;     /* the STM32's lines never reach the ESP32 */
    /* The ESP32's replies on their way to the STM32, delivered only once
     * bridge_link_feed() has returned, as on the hardware: the STM32 reads
     * them later, from its own main loop. */
    char wire[256];
    size_t wire_length;
    size_t wire_read;
    bool delivering;
    BridgeEvent events[MAX_OUT];
    int event_count;
    BridgeStatus statuses[MAX_OUT];
    int status_count;
    uint32_t acks[MAX_OUT];
    int ack_count;
} Rig;

/* The STM32 reads the replies on the wire. An ACK it reads may make it
 * send the next line at once (stm32_writes(), below), whose replies join
 * the wire and are read by this same loop, not a nested one. */
static void deliver_replies(Rig *rig)
{
    if (rig->delivering) {
        return;
    }
    rig->delivering = true;
    while (rig->wire_read < rig->wire_length) {
        box_rx_feed(&rig->rx, (uint8_t)rig->wire[rig->wire_read++]);
    }
    rig->wire_read = 0;
    rig->wire_length = 0;
    rig->delivering = false;
}

/* The STM32 writes a line: it goes over the wire to the ESP32, and then
 * the ESP32's replies go back. */
static void stm32_writes(void *context, const char *line)
{
    Rig *rig = context;
    if (!rig->lose_lines) {
        while (*line != '\0') {
            bridge_link_feed(&rig->link, (uint8_t)*line++);
        }
    }
    deliver_replies(rig);
}

/* The ESP32 replies: it goes on the wire to the STM32. */
static void esp32_replies(void *context, const char *line)
{
    Rig *rig = context;
    size_t length = strlen(line);
    if (rig->lose_replies) {
        return;
    }
    CHECK(rig->wire_length + length <= sizeof rig->wire);
    if (rig->wire_length + length <= sizeof rig->wire) {
        memcpy(&rig->wire[rig->wire_length], line, length);
        rig->wire_length += length;
    }
}

static void record_event(void *context, BridgeEvent event)
{
    Rig *rig = context;
    if (rig->event_count < MAX_OUT) {
        rig->events[rig->event_count] = event;
    }
    rig->event_count++;
}

static void record_status(void *context, BridgeStatus status)
{
    Rig *rig = context;
    if (rig->status_count < MAX_OUT) {
        rig->statuses[rig->status_count] = status;
    }
    rig->status_count++;
}

static void record_ack(void *context, uint32_t seq)
{
    Rig *rig = context;
    if (rig->ack_count < MAX_OUT) {
        rig->acks[rig->ack_count] = seq;
    }
    rig->ack_count++;
}

/* Each ACK the STM32 reads also goes to its Event sender. */
static void sender_gets_ack(void *context, uint32_t seq)
{
    Rig *rig = context;
    record_ack(context, seq);
    box_sender_ack(&rig->sender, seq, rig->now_ms);
}

/* Both ends, wired together. With `with_sender`, the ACKs the STM32 reads
 * also go to its Event sender; the test opens the session itself with
 * box_sender_start(). */
static void start_rig(Rig *rig, bool with_sender)
{
    BridgeLinkOutputs link_outputs;
    BoxRxOutputs rx_outputs;
    BoxLinesOutput lines_output;
    BoxSenderOutputs sender_outputs;

    memset(rig, 0, sizeof *rig);
    memset(&link_outputs, 0, sizeof link_outputs);
    link_outputs.context = rig;
    link_outputs.write_line = esp32_replies;
    link_outputs.on_event = record_event;
    link_outputs.on_status = record_status;
    bridge_link_init(&rig->link, link_outputs);

    memset(&rx_outputs, 0, sizeof rx_outputs);
    rx_outputs.context = rig;
    rx_outputs.on_ack = with_sender ? sender_gets_ack : record_ack;
    box_rx_init(&rig->rx, rx_outputs);

    lines_output.context = rig;
    lines_output.write_line = stm32_writes;
    box_lines_init(&rig->lines, lines_output);

    memset(&sender_outputs, 0, sizeof sender_outputs);
    sender_outputs.context = rig;
    sender_outputs.write_line = stm32_writes;
    box_sender_init(&rig->sender, sender_outputs);
}

static void start(Rig *rig)
{
    start_rig(rig, false);
}

static void start_with_sender(Rig *rig)
{
    start_rig(rig, true);
}

static void test_the_stm32s_boot_line_gets_the_ack_that_opens_its_session(void)
{
    Rig rig;
    start(&rig);
    stm32_writes(&rig, BOX_BOOT_LINE);
    CHECK_EQ_INT(1, rig.ack_count);
    CHECK_EQ_INT(BOX_BOOT_SEQ, rig.acks[0]);
    CHECK_EQ_INT(0, rig.event_count);
}

static void test_every_event_the_stm32_sends_is_acked_and_forwarded(void)
{
    static const struct {
        BoxEvent sent;
        uint32_t seq;
        BridgeOutcome forwarded;
    } cases[] = {
        { BOX_EVENT_DELIVERED,       1u,          BRIDGE_DELIVERED },
        { BOX_EVENT_COLLECTED,       2u,          BRIDGE_COLLECTED },
        { BOX_EVENT_OPENED_OCCUPIED, 3u,          BRIDGE_OPENED_OCCUPIED },
        { BOX_EVENT_OPENED_EMPTY,    4294967295u, BRIDGE_OPENED_EMPTY },
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Rig rig;
        char line[BOX_LINE_MAX];
        start(&rig);
        box_lines_format_event(line, cases[i].seq, cases[i].sent);
        stm32_writes(&rig, line);
        CHECK_EQ_INT(1, rig.ack_count);
        CHECK(rig.acks[0] == cases[i].seq);
        CHECK_EQ_INT(1, rig.event_count);
        CHECK(rig.events[0].seq == cases[i].seq);
        CHECK_EQ_INT(cases[i].forwarded, rig.events[0].outcome);
    }
}

static void test_every_box_status_the_stm32_sends_is_forwarded_without_an_ack(void)
{
    static const BoxStatus sent[] = {
        { false, false }, { true, false }, { true, true }, { false, true },
    };
    Rig rig;
    size_t i;

    start(&rig);
    for (i = 0; i < sizeof sent / sizeof sent[0]; i++) {
        box_lines_on_box_status_changed(&rig.lines, sent[i], 1000u * (uint32_t)i);
    }
    CHECK_EQ_INT(4, rig.status_count);
    for (i = 0; i < sizeof sent / sizeof sent[0]; i++) {
        CHECK_EQ_INT(sent[i].occupied, rig.statuses[i].occupied);
        CHECK_EQ_INT(sent[i].lid_open, rig.statuses[i].lid_open);
    }
    CHECK_EQ_INT(0, rig.ack_count);
}

static void test_an_event_the_stm32_resends_after_a_lost_ack_is_forwarded_once(void)
{
    Rig rig;
    start_with_sender(&rig);
    box_sender_start(&rig.sender, rig.now_ms);

    rig.lose_replies = true;   /* the ESP32 gets EVT,1 but its ACK is lost */
    rig.now_ms = 10u;
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, rig.now_ms);
    rig.lose_replies = false;
    rig.now_ms += BOX_SENDER_RESEND_MS;
    box_sender_tick(&rig.sender, rig.now_ms);   /* resent; this ACK arrives */
    rig.now_ms += BOX_SENDER_RESEND_MS;
    box_sender_tick(&rig.sender, rig.now_ms);   /* nothing left to resend */
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, rig.now_ms);

    CHECK_EQ_INT(2, rig.event_count);
    CHECK_EQ_INT(1, rig.events[0].seq);
    CHECK_EQ_INT(BRIDGE_DELIVERED, rig.events[0].outcome);
    CHECK_EQ_INT(2, rig.events[1].seq);
    CHECK_EQ_INT(BRIDGE_COLLECTED, rig.events[1].outcome);
    CHECK_EQ_INT(3, rig.ack_count);   /* ACK,0, then ACK,1 once, then ACK,2 */
    CHECK_EQ_INT(2, rig.acks[2]);
}

static void test_an_event_lost_on_its_way_to_the_esp32_is_resent_and_forwarded_once(void)
{
    Rig rig;
    start_with_sender(&rig);
    box_sender_start(&rig.sender, rig.now_ms);

    rig.lose_lines = true;   /* EVT,1 never reaches the ESP32 */
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, rig.now_ms);
    rig.lose_lines = false;
    CHECK_EQ_INT(0, rig.event_count);
    rig.now_ms += BOX_SENDER_RESEND_MS;
    box_sender_tick(&rig.sender, rig.now_ms);   /* resent */
    rig.now_ms += BOX_SENDER_RESEND_MS;
    box_sender_tick(&rig.sender, rig.now_ms);   /* ACKed: nothing to resend */

    CHECK_EQ_INT(1, rig.event_count);
    CHECK_EQ_INT(1, rig.events[0].seq);
    CHECK_EQ_INT(BRIDGE_DELIVERED, rig.events[0].outcome);
    CHECK_EQ_INT(2, rig.ack_count);   /* ACK,0, then ACK,1 */
    CHECK_EQ_INT(1, rig.acks[1]);
}

static void test_events_waiting_in_the_outbox_are_forwarded_once_each_in_order(void)
{
    Rig rig;
    start_with_sender(&rig);

    rig.lose_replies = true;   /* BOOT's ACK,0 is lost, so Events wait */
    box_sender_start(&rig.sender, rig.now_ms);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, rig.now_ms);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, rig.now_ms);
    rig.lose_replies = false;
    rig.now_ms += BOX_SENDER_RESEND_MS;
    box_sender_tick(&rig.sender, rig.now_ms);   /* BOOT again: all go out */

    CHECK_EQ_INT(2, rig.event_count);
    CHECK_EQ_INT(1, rig.events[0].seq);
    CHECK_EQ_INT(BRIDGE_DELIVERED, rig.events[0].outcome);
    CHECK_EQ_INT(2, rig.events[1].seq);
    CHECK_EQ_INT(BRIDGE_COLLECTED, rig.events[1].outcome);
    CHECK_EQ_INT(3, rig.ack_count);   /* ACK,0, ACK,1, ACK,2 */
    CHECK_EQ_INT(0, rig.acks[0]);
    CHECK_EQ_INT(1, rig.acks[1]);
    CHECK_EQ_INT(2, rig.acks[2]);

    /* The next Event is accepted, so the ESP32's session is in step. */
    box_sender_add(&rig.sender, BOX_EVENT_OPENED_EMPTY, rig.now_ms);
    CHECK_EQ_INT(3, rig.event_count);
    CHECK_EQ_INT(3, rig.events[2].seq);
}

int main(void)
{
    RUN_TEST(test_the_stm32s_boot_line_gets_the_ack_that_opens_its_session);
    RUN_TEST(test_every_event_the_stm32_sends_is_acked_and_forwarded);
    RUN_TEST(test_every_box_status_the_stm32_sends_is_forwarded_without_an_ack);
    RUN_TEST(test_an_event_the_stm32_resends_after_a_lost_ack_is_forwarded_once);
    RUN_TEST(test_an_event_lost_on_its_way_to_the_esp32_is_resent_and_forwarded_once);
    RUN_TEST(test_events_waiting_in_the_outbox_are_forwarded_once_each_in_order);
    return TEST_SUMMARY();
}
