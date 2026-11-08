/*
 * PC tests of the UART protocol across both boards: the STM32's line writer
 * (box_lines) feeds the ESP32's link (bridge_link), and the ESP32's replies
 * go back through the STM32's line reader (box_rx). A change to a line on
 * one side only (a renamed outcome, a reordered field) fails here.
 */
#include <string.h>

#include "box_lines.h"
#include "box_rx.h"
#include "bridge_link.h"
#include "test_harness.h"

#define MAX_OUT 8

typedef struct {
    BridgeLink link;     /* the ESP32 end */
    BoxRx rx;            /* the STM32 end, reading the ESP32's replies */
    BoxLines lines;      /* the STM32 end, writing STS lines */
    BridgeEvent events[MAX_OUT];
    int event_count;
    BridgeStatus statuses[MAX_OUT];
    int status_count;
    uint32_t acks[MAX_OUT];
    int ack_count;
} Rig;

/* The STM32 writes a line: it goes over the wire to the ESP32. */
static void stm32_writes(void *context, const char *line)
{
    Rig *rig = context;
    while (*line != '\0') {
        bridge_link_feed(&rig->link, (uint8_t)*line++);
    }
}

/* The ESP32 replies: it goes over the wire to the STM32. */
static void esp32_replies(void *context, const char *line)
{
    Rig *rig = context;
    while (*line != '\0') {
        box_rx_feed(&rig->rx, (uint8_t)*line++);
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

static void start(Rig *rig)
{
    BridgeLinkOutputs link_outputs;
    BoxRxOutputs rx_outputs;
    BoxLinesOutput lines_output;

    memset(rig, 0, sizeof *rig);
    memset(&link_outputs, 0, sizeof link_outputs);
    link_outputs.context = rig;
    link_outputs.write_line = esp32_replies;
    link_outputs.on_event = record_event;
    link_outputs.on_status = record_status;
    bridge_link_init(&rig->link, link_outputs);

    memset(&rx_outputs, 0, sizeof rx_outputs);
    rx_outputs.context = rig;
    rx_outputs.on_ack = record_ack;
    box_rx_init(&rig->rx, rx_outputs);

    lines_output.context = rig;
    lines_output.write_line = stm32_writes;
    box_lines_init(&rig->lines, lines_output);
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

int main(void)
{
    RUN_TEST(test_the_stm32s_boot_line_gets_the_ack_that_opens_its_session);
    RUN_TEST(test_every_event_the_stm32_sends_is_acked_and_forwarded);
    RUN_TEST(test_every_box_status_the_stm32_sends_is_forwarded_without_an_ack);
    return TEST_SUMMARY();
}
