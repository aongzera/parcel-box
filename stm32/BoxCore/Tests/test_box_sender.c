/*
 * PC tests for the Event sender (box_sender). Each test adds Events, feeds
 * ACKs and ticks, then checks only the EVT lines it wrote.
 */
#include <string.h>

#include "box_sender.h"
#include "test_harness.h"

#define MAX_LINES 256
#define LINE_LEN 64
#define MAX_DROPS 16

typedef struct {
    char lines[MAX_LINES][LINE_LEN];
    int count;
    uint32_t dropped_seqs[MAX_DROPS];
    int drop_count;
} Recorder;

static void record_line(void *context, const char *line)
{
    Recorder *r = context;
    if (r->count < MAX_LINES) {
        strncpy(r->lines[r->count], line, LINE_LEN - 1);
        r->lines[r->count][LINE_LEN - 1] = '\0';
    }
    r->count++;
}

static void record_drop(void *context, uint32_t seq, BoxEvent event)
{
    Recorder *r = context;
    (void)event;
    if (r->drop_count < MAX_DROPS) {
        r->dropped_seqs[r->drop_count] = seq;
    }
    r->drop_count++;
}

typedef struct {
    BoxSender sender;
    Recorder recorder;
} Rig;

static void start(Rig *rig)
{
    BoxSenderOutputs outputs;
    memset(rig, 0, sizeof *rig);
    outputs.context = &rig->recorder;
    outputs.write_line = record_line;
    outputs.on_dropped = record_drop;
    box_sender_init(&rig->sender, outputs);
}

#define CHECK_LINE(rig, index, expected)                                  \
    do {                                                                  \
        CHECK((index) < MAX_LINES && (rig)->recorder.count > (index));    \
        if ((index) < MAX_LINES && (rig)->recorder.count > (index) &&     \
            strcmp((rig)->recorder.lines[(index)], (expected)) != 0) {    \
            printf("    %s:%d: line %d: expected \"%s\", got \"%s\"\n",   \
                   __FILE__, __LINE__, (index), (expected),               \
                   (rig)->recorder.lines[(index)]);                       \
            test_current_failed = 1;                                      \
        }                                                                 \
    } while (0)

/* ---- Tests --------------------------------------------------------------- */

static void test_an_event_is_sent_at_once_with_sequence_number_1(void)
{
    Rig rig;
    start(&rig);

    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);

    CHECK_EQ_INT(1, rig.recorder.count);
    CHECK_LINE(&rig, 0, "EVT,1,DELIVERED\n");
}

/* Tick every 10 ms from `from_ms` up to and including `to_ms`. Counts
 * elapsed time, so it is safe across the millisecond wraparound. */
static void tick_through(Rig *rig, uint32_t from_ms, uint32_t to_ms)
{
    uint32_t now_ms = from_ms;
    for (;;) {
        box_sender_tick(&rig->sender, now_ms);
        if (now_ms == to_ms) {
            break;
        }
        now_ms += 10u;
    }
}

static void test_an_unacked_event_is_resent_every_resend_interval(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, 5000);

    tick_through(&rig, 5000, 5000 + BOX_SENDER_RESEND_MS - 10);
    CHECK_EQ_INT(1, rig.recorder.count);

    tick_through(&rig, 5000 + BOX_SENDER_RESEND_MS, 5000 + 3 * BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(4, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,1,COLLECTED\n");
    CHECK_LINE(&rig, 2, "EVT,1,COLLECTED\n");
    CHECK_LINE(&rig, 3, "EVT,1,COLLECTED\n");
}

static void test_an_event_is_not_sent_again_after_its_ack(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);

    box_sender_ack(&rig.sender, 1, 5020);
    tick_through(&rig, 5020, 5020 + 10 * BOX_SENDER_RESEND_MS);

    CHECK_EQ_INT(1, rig.recorder.count);
}

static void test_an_ack_for_another_sequence_number_is_ignored(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);

    box_sender_ack(&rig.sender, 2, 5020);
    box_sender_ack(&rig.sender, 0, 5030);
    tick_through(&rig, 5030, 5000 + BOX_SENDER_RESEND_MS);

    CHECK_EQ_INT(2, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,1,DELIVERED\n");
}

static void test_events_leave_in_order_one_at_a_time(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);
    box_sender_add(&rig.sender, BOX_EVENT_OPENED_OCCUPIED, 6000);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, 7000);

    /* Only the oldest goes out while it waits for its ACK. */
    CHECK_EQ_INT(1, rig.recorder.count);
    CHECK_LINE(&rig, 0, "EVT,1,DELIVERED\n");

    box_sender_ack(&rig.sender, 1, 7010);
    CHECK_EQ_INT(2, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,2,OPENED_OCCUPIED\n");

    box_sender_ack(&rig.sender, 2, 7020);
    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 2, "EVT,3,COLLECTED\n");

    box_sender_ack(&rig.sender, 3, 7030);
    tick_through(&rig, 7030, 7030 + 5 * BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(3, rig.recorder.count);
}

static void test_the_next_event_gets_the_full_resend_interval(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, 5100);
    tick_through(&rig, 5000, 5900);

    box_sender_ack(&rig.sender, 1, 5900);
    tick_through(&rig, 5900, 5900 + BOX_SENDER_RESEND_MS - 10);
    CHECK_EQ_INT(2, rig.recorder.count);

    tick_through(&rig, 5900 + BOX_SENDER_RESEND_MS, 5900 + BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 2, "EVT,2,COLLECTED\n");
}

static void test_a_lost_ack_is_survived_by_resending_until_the_next_ack(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, 5500);

    /* The ESP32's ACK,1 for the first send is lost: no ack call. */
    tick_through(&rig, 5000, 5000 + BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(2, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,1,DELIVERED\n");

    /* The ESP32 ACKs the resend (as a duplicate it ignores but ACKs). */
    box_sender_ack(&rig.sender, 1, 6020);
    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 2, "EVT,2,COLLECTED\n");
}

static void test_a_duplicate_ack_is_harmless(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, 5100);
    tick_through(&rig, 5000, 5000 + BOX_SENDER_RESEND_MS);   /* resent once */

    box_sender_ack(&rig.sender, 1, 6010);   /* ACK of the first send */
    box_sender_ack(&rig.sender, 1, 6020);   /* ACK of the resend */

    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 2, "EVT,2,COLLECTED\n");
    /* Event 2 is still in flight, so it is still resent. */
    tick_through(&rig, 6020, 6010 + BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(4, rig.recorder.count);
    CHECK_LINE(&rig, 3, "EVT,2,COLLECTED\n");
}

static void test_resending_keeps_time_across_the_millisecond_wraparound(void)
{
    Rig rig;
    uint32_t sent_at_ms = 0xFFFFFFFFu - 495u;   /* a multiple of 10 ms */
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_OPENED_EMPTY, sent_at_ms);

    tick_through(&rig, sent_at_ms, sent_at_ms + BOX_SENDER_RESEND_MS - 10u);
    CHECK_EQ_INT(1, rig.recorder.count);

    tick_through(&rig, sent_at_ms + BOX_SENDER_RESEND_MS, sent_at_ms + BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(2, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,1,OPENED_EMPTY\n");
}

/* A tick stamped a few ms before the ACK that sent the next Event (the main
 * loop read its clock before the ACK arrived) is not a resend: the Event
 * has only just been sent. */
static void test_a_tick_stamped_just_before_the_send_does_not_resend(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, 5100);

    box_sender_ack(&rig.sender, 1, 6003);   /* sends Event 2 at 6003 */
    box_sender_tick(&rig.sender, 6000);
    CHECK_EQ_INT(2, rig.recorder.count);

    tick_through(&rig, 6010, 6003 + BOX_SENDER_RESEND_MS - 3);
    CHECK_EQ_INT(2, rig.recorder.count);
    tick_through(&rig, 6003 + BOX_SENDER_RESEND_MS + 7, 6003 + BOX_SENDER_RESEND_MS + 7);
    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 2, "EVT,2,COLLECTED\n");
}

/* Ack the Event in flight and return the last line written (the next
 * Event's, if one was sent), or "" if nothing has been written. */
static const char *ack_in_flight(Rig *rig, uint32_t seq, uint32_t now_ms)
{
    box_sender_ack(&rig->sender, seq, now_ms);
    if (rig->recorder.count < 1 || rig->recorder.count > MAX_LINES) {
        return "";
    }
    return rig->recorder.lines[rig->recorder.count - 1];
}

static void test_a_full_outbox_drops_the_oldest_event_waiting_behind_the_one_in_flight(void)
{
    Rig rig;
    uint32_t i;
    start(&rig);
    /* The link is down: nothing is ACKed. Fill the outbox, then one more. */
    for (i = 0; i < BOX_SENDER_CAPACITY + 1u; i++) {
        box_sender_add(&rig.sender, BOX_EVENT_OPENED_EMPTY, 5000u + 100u * i);
    }

    CHECK_EQ_INT(1, rig.recorder.drop_count);
    CHECK_EQ_INT(2, rig.recorder.dropped_seqs[0]);

    /* The link comes back: Event 1 (in flight), then 3 up to the newest,
     * in order, each sent once. */
    CHECK_LINE(&rig, 0, "EVT,1,OPENED_EMPTY\n");
    CHECK(strcmp(ack_in_flight(&rig, 1, 9000), "EVT,3,OPENED_EMPTY\n") == 0);
    CHECK(strcmp(ack_in_flight(&rig, 3, 9010), "EVT,4,OPENED_EMPTY\n") == 0);
    for (i = 4; i <= BOX_SENDER_CAPACITY + 1u; i++) {
        box_sender_ack(&rig.sender, i, 9020u + i);
    }
    CHECK_EQ_INT(BOX_SENDER_CAPACITY, rig.recorder.count);
    CHECK_LINE(&rig, (int)BOX_SENDER_CAPACITY - 1, "EVT,9,OPENED_EMPTY\n");
    tick_through(&rig, 9100, 9100 + 5 * BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(BOX_SENDER_CAPACITY, rig.recorder.count);
}

static void test_the_event_in_flight_is_never_dropped(void)
{
    Rig rig;
    uint32_t i;
    start(&rig);
    for (i = 0; i < 3u * BOX_SENDER_CAPACITY; i++) {
        box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000u + 100u * i);
    }
    tick_through(&rig, 5000, 5000 + BOX_SENDER_RESEND_MS * 4);

    CHECK_EQ_INT(2 * BOX_SENDER_CAPACITY, rig.recorder.drop_count);
    CHECK_LINE(&rig, rig.recorder.count - 1, "EVT,1,DELIVERED\n");
}

/* ---- Wired as the firmware wires it (main.c) ------------------------------ */

/* The core's outputs share box_lines' context, so on_event finds the sender
 * here, as main.c finds its static one. */
static BoxSender *wired_sender;

static void wired_on_event(void *context, BoxEvent event, uint32_t now_ms)
{
    (void)context;
    box_sender_add(wired_sender, event, now_ms);
}

static BoxReadings readings(bool lid_open, bool parcel_present)
{
    BoxReadings r;
    r.lid_open = lid_open;
    r.parcel_present = parcel_present;
    return r;
}

typedef struct {
    Rig rig;            /* the sender, and the recorder every line goes to */
    BoxLines lines;
    BoxCore core;
} Firmware;

static uint32_t run_firmware(Firmware *fw, uint32_t now_ms, BoxReadings r,
                             uint32_t duration_ms)
{
    uint32_t elapsed_ms;
    for (elapsed_ms = 0; elapsed_ms < duration_ms; elapsed_ms += 10u) {
        now_ms += 10u;
        box_core_tick(&fw->core, now_ms, r);
        box_lines_tick(&fw->lines, now_ms);
        box_sender_tick(&fw->rig.sender, now_ms);
    }
    return now_ms;
}

static void test_wired_to_the_core_a_delivery_is_resent_until_acked(void)
{
    static Firmware fw;
    BoxLinesOutput lines_output;
    BoxCoreOutputs core_outputs;
    uint32_t now_ms = 0;
    start(&fw.rig);
    wired_sender = &fw.rig.sender;
    lines_output.context = &fw.rig.recorder;
    lines_output.write_line = record_line;
    box_lines_init(&fw.lines, lines_output);
    core_outputs = box_lines_core_outputs(&fw.lines);
    core_outputs.on_event = wired_on_event;
    box_core_init(&fw.core, core_outputs, now_ms, readings(false, false));

    now_ms = run_firmware(&fw, now_ms, readings(false, false), BOX_SETTLE_TIME_MS + 100);
    now_ms = run_firmware(&fw, now_ms, readings(true, false), 1000);
    now_ms = run_firmware(&fw, now_ms, readings(false, true), BOX_SETTLE_TIME_MS + 100);
    CHECK_EQ_INT(5, fw.rig.recorder.count);
    CHECK_LINE(&fw.rig, 3, "STS,OCCUPIED,LID_CLOSED\n");
    CHECK_LINE(&fw.rig, 4, "EVT,1,DELIVERED\n");

    now_ms = run_firmware(&fw, now_ms, readings(false, true), BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(6, fw.rig.recorder.count);
    CHECK_LINE(&fw.rig, 5, "EVT,1,DELIVERED\n");

    box_sender_ack(&fw.rig.sender, 1, now_ms);
    /* From here on only the STS heartbeat. */
    run_firmware(&fw, now_ms, readings(false, true), BOX_STS_HEARTBEAT_MS);
    CHECK_EQ_INT(7, fw.rig.recorder.count);
    CHECK_LINE(&fw.rig, 6, "STS,OCCUPIED,LID_CLOSED\n");
}

int main(void)
{
    RUN_TEST(test_an_event_is_sent_at_once_with_sequence_number_1);
    RUN_TEST(test_an_unacked_event_is_resent_every_resend_interval);
    RUN_TEST(test_an_event_is_not_sent_again_after_its_ack);
    RUN_TEST(test_an_ack_for_another_sequence_number_is_ignored);
    RUN_TEST(test_events_leave_in_order_one_at_a_time);
    RUN_TEST(test_the_next_event_gets_the_full_resend_interval);
    RUN_TEST(test_a_lost_ack_is_survived_by_resending_until_the_next_ack);
    RUN_TEST(test_a_duplicate_ack_is_harmless);
    RUN_TEST(test_resending_keeps_time_across_the_millisecond_wraparound);
    RUN_TEST(test_a_tick_stamped_just_before_the_send_does_not_resend);
    RUN_TEST(test_a_full_outbox_drops_the_oldest_event_waiting_behind_the_one_in_flight);
    RUN_TEST(test_the_event_in_flight_is_never_dropped);
    RUN_TEST(test_wired_to_the_core_a_delivery_is_resent_until_acked);
    return TEST_SUMMARY();
}
