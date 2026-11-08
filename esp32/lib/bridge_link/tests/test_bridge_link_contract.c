/*
 * PC tests of the UART protocol across both boards: the STM32's line writer
 * (box_lines) feeds the ESP32's link (bridge_link), and the ESP32's replies
 * go back through the STM32's line reader (box_rx). A change to a line on
 * one side only (a renamed outcome, a reordered field, the checksum rule)
 * fails here.
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
    /* The next line the STM32 writes has bits `garble_mask` of its byte
     * `garble_at` flipped on the wire, as line noise would. */
    bool garble_next_line;
    size_t garble_at;
    uint8_t garble_mask;
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
    size_t i;
    if (!rig->lose_lines) {
        for (i = 0; line[i] != '\0'; i++) {
            uint8_t byte = (uint8_t)line[i];
            if (rig->garble_next_line && i == rig->garble_at) {
                byte ^= rig->garble_mask;
            }
            bridge_link_feed(&rig->link, byte);
        }
    }
    rig->garble_next_line = false;
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

/* The STM32 end, as it is after power-up or a reset: its line reader, line
 * writer and Event sender start afresh. With `with_sender`, the ACKs it
 * reads also go to its Event sender. */
static void init_stm32_end(Rig *rig, bool with_sender)
{
    BoxRxOutputs rx_outputs;
    BoxLinesOutput lines_output;
    BoxSenderOutputs sender_outputs;

    memset(&rx_outputs, 0, sizeof rx_outputs);
    rx_outputs.context = rig;
    rx_outputs.on_ack = with_sender ? sender_gets_ack : record_ack;
    box_rx_init(&rig->rx, rx_outputs);

    memset(&lines_output, 0, sizeof lines_output);
    lines_output.context = rig;
    lines_output.write_line = stm32_writes;
    box_lines_init(&rig->lines, lines_output);

    memset(&sender_outputs, 0, sizeof sender_outputs);
    sender_outputs.context = rig;
    sender_outputs.write_line = stm32_writes;
    box_sender_init(&rig->sender, sender_outputs);
}

/* Both ends, wired together. With `with_sender`, the ACKs the STM32 reads
 * also go to its Event sender; the test opens the session itself with
 * box_sender_start(). */
static void start_rig(Rig *rig, bool with_sender)
{
    BridgeLinkOutputs link_outputs;

    memset(rig, 0, sizeof *rig);
    memset(&link_outputs, 0, sizeof link_outputs);
    link_outputs.context = rig;
    link_outputs.write_line = esp32_replies;
    link_outputs.on_event = record_event;
    link_outputs.on_status = record_status;
    bridge_link_init(&rig->link, link_outputs);

    init_stm32_end(rig, with_sender);
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

/* --- STM32 resets (#29) ------------------------------------------------ */

/* The STM32 resets: its line reader, line writer and Event sender start
 * afresh, and its clock restarts. The ESP32 keeps running with the same
 * link. */
static void reset_stm32(Rig *rig)
{
    init_stm32_end(rig, true);
    rig->now_ms = 0;
    rig->wire_length = 0;   /* replies in flight are lost with the reset */
    rig->wire_read = 0;
}

/* A session that opens and sends `count` Events, each ACKed at once. */
static void run_session(Rig *rig, int count)
{
    int i;
    box_sender_start(&rig->sender, rig->now_ms);
    for (i = 0; i < count; i++) {
        rig->now_ms += 10u;
        box_sender_add(&rig->sender, BOX_EVENT_DELIVERED, rig->now_ms);
    }
}

static void test_after_an_stm32_reset_the_new_sessions_events_are_forwarded(void)
{
    /* One Event before the reset is the case the old "lower seq" rule got
     * wrong: the new EVT,1 has the same number as the last one accepted. */
    static const int before_reset[] = { 1, 3 };
    size_t i;

    for (i = 0; i < sizeof before_reset / sizeof before_reset[0]; i++) {
        Rig rig;
        start_with_sender(&rig);
        run_session(&rig, before_reset[i]);
        CHECK_EQ_INT(before_reset[i], rig.event_count);

        reset_stm32(&rig);
        box_sender_start(&rig.sender, rig.now_ms);
        box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, rig.now_ms);
        box_sender_add(&rig.sender, BOX_EVENT_OPENED_EMPTY, rig.now_ms);

        CHECK_EQ_INT(before_reset[i] + 2, rig.event_count);
        CHECK_EQ_INT(1, rig.events[before_reset[i]].seq);
        CHECK_EQ_INT(BRIDGE_COLLECTED, rig.events[before_reset[i]].outcome);
        CHECK_EQ_INT(2, rig.events[before_reset[i] + 1].seq);
        CHECK_EQ_INT(BRIDGE_OPENED_EMPTY, rig.events[before_reset[i] + 1].outcome);
    }
}

static void test_an_stm32_reset_while_an_events_ack_is_lost_does_not_skip_the_new_event_1(void)
{
    Rig rig;
    start_with_sender(&rig);
    box_sender_start(&rig.sender, rig.now_ms);
    rig.lose_replies = true;   /* the ESP32 accepts EVT,1; its ACK is lost */
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, rig.now_ms);
    rig.lose_replies = false;

    reset_stm32(&rig);   /* before EVT,1 is resent */
    box_sender_start(&rig.sender, rig.now_ms);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, rig.now_ms);

    CHECK_EQ_INT(2, rig.event_count);
    CHECK_EQ_INT(1, rig.events[1].seq);
    CHECK_EQ_INT(BRIDGE_COLLECTED, rig.events[1].outcome);
}

static void test_an_stm32_reset_whose_boot_ack_is_lost_still_forwards_the_new_event_1(void)
{
    Rig rig;
    start_with_sender(&rig);
    run_session(&rig, 2);

    reset_stm32(&rig);
    rig.lose_replies = true;   /* the ESP32 gets BOOT; its ACK,0 is lost */
    box_sender_start(&rig.sender, rig.now_ms);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, rig.now_ms);   /* waits */
    rig.lose_replies = false;
    CHECK_EQ_INT(2, rig.event_count);
    rig.now_ms += BOX_SENDER_RESEND_MS;
    box_sender_tick(&rig.sender, rig.now_ms);   /* BOOT again, then EVT,1 */

    CHECK_EQ_INT(3, rig.event_count);
    CHECK_EQ_INT(1, rig.events[2].seq);
    CHECK_EQ_INT(BRIDGE_COLLECTED, rig.events[2].outcome);
}

static void test_a_late_ack_from_before_an_stm32_reset_does_not_lose_the_new_event_1(void)
{
    static const char late_ack[] = "ACK,1*6E\n";
    Rig rig;
    size_t i;

    start_with_sender(&rig);
    box_sender_start(&rig.sender, rig.now_ms);
    rig.lose_replies = true;   /* EVT,1 is accepted; its ACK is delayed */
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, rig.now_ms);

    reset_stm32(&rig);
    box_sender_start(&rig.sender, rig.now_ms);   /* ACK,0 is lost too */
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, rig.now_ms);   /* waits */
    rig.lose_replies = false;
    for (i = 0; late_ack[i] != '\0'; i++) {   /* the old session's ACK,1 */
        box_rx_feed(&rig.rx, (uint8_t)late_ack[i]);
    }
    rig.now_ms += BOX_SENDER_RESEND_MS;
    box_sender_tick(&rig.sender, rig.now_ms);   /* BOOT again, then EVT,1 */

    CHECK_EQ_INT(2, rig.event_count);
    CHECK_EQ_INT(1, rig.events[1].seq);
    CHECK_EQ_INT(BRIDGE_COLLECTED, rig.events[1].outcome);
}

static void test_after_an_stm32_reset_a_resent_event_is_still_forwarded_once(void)
{
    Rig rig;
    start_with_sender(&rig);
    run_session(&rig, 1);

    reset_stm32(&rig);
    box_sender_start(&rig.sender, rig.now_ms);
    rig.lose_replies = true;   /* the new EVT,1 is accepted; its ACK is lost */
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, rig.now_ms);
    rig.lose_replies = false;
    rig.now_ms += BOX_SENDER_RESEND_MS;
    box_sender_tick(&rig.sender, rig.now_ms);   /* EVT,1 resent: ACKed only */
    box_sender_add(&rig.sender, BOX_EVENT_OPENED_EMPTY, rig.now_ms);

    CHECK_EQ_INT(3, rig.event_count);
    CHECK_EQ_INT(1, rig.events[1].seq);
    CHECK_EQ_INT(BRIDGE_COLLECTED, rig.events[1].outcome);
    CHECK_EQ_INT(2, rig.events[2].seq);
    CHECK_EQ_INT(BRIDGE_OPENED_EMPTY, rig.events[2].outcome);
}

static void test_an_event_line_cut_off_by_an_stm32_reset_is_dropped_and_the_new_event_1_forwarded(void)
{
    static const char cut[] = "EVT,2,DELI";   /* the reset came mid-line */
    static const BoxStatus starting = { false, false };
    Rig rig;
    size_t i;

    start_with_sender(&rig);
    run_session(&rig, 1);   /* ACK,0 and ACK,1 */
    for (i = 0; cut[i] != '\0'; i++) {
        bridge_link_feed(&rig.link, (uint8_t)cut[i]);
    }

    reset_stm32(&rig);
    /* The first STS joins the cut line: "EVT,2,DELISTS,EMPTY,LID_CLOSED*A5". */
    box_lines_on_box_status_changed(&rig.lines, starting, rig.now_ms);
    box_sender_start(&rig.sender, rig.now_ms);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, rig.now_ms);

    CHECK_EQ_INT(2, rig.event_count);   /* the old EVT,1 and the new one */
    CHECK_EQ_INT(1, rig.events[1].seq);
    CHECK_EQ_INT(BRIDGE_COLLECTED, rig.events[1].outcome);
    CHECK_EQ_INT(4, rig.ack_count);     /* no ACK for the merged line */
    CHECK_EQ_INT(0, rig.acks[2]);
    CHECK_EQ_INT(1, rig.acks[3]);
}

/* --- Checksums -------------------------------------------------------- */

static void test_both_boards_compute_the_same_checksum_for_every_event_and_ack(void)
{
    /* Each board has its own checksum code (box_checksum, bridge_link).
     * Every STM32 EVT line must check out on the ESP32, and the ESP32's
     * ACK for it on the STM32. These give many checksum values, with
     * every digit in every place up to four digits, and the longest. */
    static const BoxEvent outcomes[] = {
        BOX_EVENT_DELIVERED, BOX_EVENT_COLLECTED,
        BOX_EVENT_OPENED_OCCUPIED, BOX_EVENT_OPENED_EMPTY,
    };
    static const uint32_t big[] = { 4294967295u, 4294967294u, 1234567890u, 987654321u };
    uint32_t seq;
    size_t o;
    int mismatches = 0;

    for (o = 0; o < sizeof outcomes / sizeof outcomes[0]; o++) {
        for (seq = 1u; seq <= 2100u + sizeof big / sizeof big[0]; seq++) {
            uint32_t sent = seq <= 2100u ? seq : big[seq - 2101u];
            Rig rig;
            char line[BOX_LINE_MAX];
            start(&rig);
            box_lines_format_event(line, sent, outcomes[o]);
            stm32_writes(&rig, line);
            if (rig.event_count != 1 || rig.ack_count != 1 || rig.acks[0] != sent) {
                if (mismatches++ < 5) {
                    printf("    \"%.*s\" not read back as Event and ACK %lu\n",
                           (int)strlen(line) - 1, line, (unsigned long)sent);
                }
            }
        }
    }
    CHECK_EQ_INT(0, mismatches);
}

static void test_an_event_garbled_on_the_wire_is_dropped_then_resent_and_forwarded(void)
{
    Rig rig;
    start_with_sender(&rig);
    box_sender_start(&rig.sender, rig.now_ms);

    /* "EVT,1,..." arrives as "EVT,9,...": one bit (0x08) flipped. */
    rig.garble_next_line = true;
    rig.garble_at = 4;
    rig.garble_mask = 0x08;
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, rig.now_ms);
    CHECK_EQ_INT(0, rig.event_count);
    CHECK_EQ_INT(1, rig.ack_count);   /* ACK,0 only: nothing for EVT,9 */

    rig.now_ms += BOX_SENDER_RESEND_MS;
    box_sender_tick(&rig.sender, rig.now_ms);   /* resent, intact */
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, rig.now_ms);

    /* Event 9 was never accepted, so Event 2 isn't taken for a resend. */
    CHECK_EQ_INT(2, rig.event_count);
    CHECK_EQ_INT(1, rig.events[0].seq);
    CHECK_EQ_INT(2, rig.events[1].seq);
}

int main(void)
{
    RUN_TEST(test_the_stm32s_boot_line_gets_the_ack_that_opens_its_session);
    RUN_TEST(test_every_event_the_stm32_sends_is_acked_and_forwarded);
    RUN_TEST(test_every_box_status_the_stm32_sends_is_forwarded_without_an_ack);
    RUN_TEST(test_an_event_the_stm32_resends_after_a_lost_ack_is_forwarded_once);
    RUN_TEST(test_an_event_lost_on_its_way_to_the_esp32_is_resent_and_forwarded_once);
    RUN_TEST(test_events_waiting_in_the_outbox_are_forwarded_once_each_in_order);
    RUN_TEST(test_after_an_stm32_reset_the_new_sessions_events_are_forwarded);
    RUN_TEST(test_an_stm32_reset_while_an_events_ack_is_lost_does_not_skip_the_new_event_1);
    RUN_TEST(test_an_stm32_reset_whose_boot_ack_is_lost_still_forwards_the_new_event_1);
    RUN_TEST(test_a_late_ack_from_before_an_stm32_reset_does_not_lose_the_new_event_1);
    RUN_TEST(test_after_an_stm32_reset_a_resent_event_is_still_forwarded_once);
    RUN_TEST(test_an_event_line_cut_off_by_an_stm32_reset_is_dropped_and_the_new_event_1_forwarded);
    RUN_TEST(test_both_boards_compute_the_same_checksum_for_every_event_and_ack);
    RUN_TEST(test_an_event_garbled_on_the_wire_is_dropped_then_resent_and_forwarded);
    return TEST_SUMMARY();
}
