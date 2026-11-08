/*
 * PC tests for the ESP32's offline Event queue (event_queue). Each test
 * pushes Events as bridge_link's on_event would, ticks the queue as the
 * main loop would, and checks only what comes out: the uploads (in order,
 * with the time each Event arrived) and the Events dropped when full.
 */
#include <string.h>

#include "bridge_link.h"
#include "event_queue.h"
#include "test_harness.h"

#define MAX_OUT 128
#define SYNCED_S 1794148325LL   /* 2026-11-08T14:32:05Z */

typedef struct {
    EventQueue queue;
    int64_t clock_s;      /* the ESP32's clock (time(nullptr)) */
    uint32_t now_ms;      /* its millis() */
    bool online;          /* upload() succeeds */
    QueuedEvent uploads[MAX_OUT];   /* each successful upload, in order */
    int upload_count;
    int attempt_count;    /* every upload() call, failed ones too */
    uint32_t dropped[MAX_OUT];      /* the seq of each dropped Event */
    int dropped_count;
} Rig;

static int64_t read_clock(void *context)
{
    return ((Rig *)context)->clock_s;
}

static bool upload(void *context, const QueuedEvent *queued)
{
    Rig *rig = context;
    rig->attempt_count++;
    if (!rig->online) {
        return false;
    }
    if (rig->upload_count < MAX_OUT) {
        rig->uploads[rig->upload_count] = *queued;
    }
    rig->upload_count++;
    return true;
}

static void record_dropped(void *context, const QueuedEvent *dropped)
{
    Rig *rig = context;
    if (rig->dropped_count < MAX_OUT) {
        rig->dropped[rig->dropped_count] = dropped->event.seq;
    }
    rig->dropped_count++;
}

static void start(Rig *rig)
{
    EventQueueOutputs outputs;
    memset(rig, 0, sizeof *rig);
    rig->clock_s = SYNCED_S;
    rig->online = true;
    memset(&outputs, 0, sizeof outputs);
    outputs.context = rig;
    outputs.clock_s = read_clock;
    outputs.upload = upload;
    outputs.on_dropped = record_dropped;
    event_queue_init(&rig->queue, outputs);
}

static void push(Rig *rig, uint32_t seq, BridgeOutcome outcome)
{
    BridgeEvent event;
    event.seq = seq;
    event.outcome = outcome;
    event_queue_push(&rig->queue, event);
}

/* The main loop runs `count` times, 10 ms apart. */
static void run_loop(Rig *rig, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        event_queue_tick(&rig->queue, rig->now_ms);
        rig->now_ms += 10u;
    }
}

static void test_an_event_is_uploaded_with_the_time_it_arrived(void)
{
    Rig rig;
    start(&rig);
    push(&rig, 1u, BRIDGE_DELIVERED);
    rig.clock_s += 3600;   /* uploaded an hour later */
    run_loop(&rig, 1);

    CHECK_EQ_INT(1, rig.upload_count);
    CHECK_EQ_INT(1, rig.uploads[0].event.seq);
    CHECK_EQ_INT(BRIDGE_DELIVERED, rig.uploads[0].event.outcome);
    CHECK(rig.uploads[0].time.known);
    CHECK(rig.uploads[0].time.unix_s == SYNCED_S);
    CHECK_EQ_INT(0, event_queue_count(&rig.queue));
}

static void test_an_event_that_arrives_before_ntp_syncs_is_uploaded_as_unknown(void)
{
    Rig rig;
    start(&rig);
    rig.clock_s = 12;   /* 12 s after boot, never synced */
    push(&rig, 1u, BRIDGE_COLLECTED);
    rig.clock_s = SYNCED_S;   /* synced before the upload: still unknown */
    run_loop(&rig, 1);

    CHECK_EQ_INT(1, rig.upload_count);
    CHECK(!rig.uploads[0].time.known);
}

static void test_an_empty_queue_uploads_nothing(void)
{
    Rig rig;
    start(&rig);
    run_loop(&rig, 5);
    CHECK_EQ_INT(0, rig.attempt_count);
}

static void test_events_are_uploaded_in_order_one_per_tick(void)
{
    Rig rig;
    start(&rig);
    push(&rig, 1u, BRIDGE_DELIVERED);
    rig.clock_s += 5;
    push(&rig, 2u, BRIDGE_OPENED_OCCUPIED);
    rig.clock_s += 5;
    push(&rig, 3u, BRIDGE_COLLECTED);

    run_loop(&rig, 1);
    CHECK_EQ_INT(1, rig.upload_count);
    CHECK_EQ_INT(2, event_queue_count(&rig.queue));
    run_loop(&rig, 2);
    CHECK_EQ_INT(3, rig.upload_count);
    CHECK_EQ_INT(1, rig.uploads[0].event.seq);
    CHECK_EQ_INT(2, rig.uploads[1].event.seq);
    CHECK_EQ_INT(BRIDGE_OPENED_OCCUPIED, rig.uploads[1].event.outcome);
    CHECK_EQ_INT(3, rig.uploads[2].event.seq);
    CHECK(rig.uploads[0].time.unix_s == SYNCED_S);
    CHECK(rig.uploads[1].time.unix_s == SYNCED_S + 5);
    CHECK(rig.uploads[2].time.unix_s == SYNCED_S + 10);
    CHECK_EQ_INT(3, rig.attempt_count);
}

static void test_a_failed_upload_keeps_the_event_and_waits_before_retrying(void)
{
    Rig rig;
    start(&rig);
    push(&rig, 1u, BRIDGE_DELIVERED);
    push(&rig, 2u, BRIDGE_COLLECTED);
    rig.online = false;
    rig.now_ms = 1000u;
    event_queue_tick(&rig.queue, rig.now_ms);   /* fails */
    CHECK_EQ_INT(1, rig.attempt_count);
    CHECK_EQ_INT(2, event_queue_count(&rig.queue));

    rig.online = true;
    event_queue_tick(&rig.queue, rig.now_ms + EVENT_QUEUE_RETRY_MS - 1u);
    CHECK_EQ_INT(1, rig.attempt_count);   /* still waiting */

    event_queue_tick(&rig.queue, rig.now_ms + EVENT_QUEUE_RETRY_MS);
    event_queue_tick(&rig.queue, rig.now_ms + EVENT_QUEUE_RETRY_MS + 10u);
    CHECK_EQ_INT(2, rig.upload_count);
    CHECK_EQ_INT(1, rig.uploads[0].event.seq);   /* nothing skipped */
    CHECK_EQ_INT(2, rig.uploads[1].event.seq);
}

static void test_the_retry_wait_works_across_the_millisecond_clock_wrapping(void)
{
    Rig rig;
    start(&rig);
    push(&rig, 1u, BRIDGE_DELIVERED);
    rig.online = false;
    event_queue_tick(&rig.queue, 0xFFFFFFFFu - 100u);   /* fails */
    rig.online = true;
    event_queue_tick(&rig.queue, 100u);   /* only 201 ms later */
    CHECK_EQ_INT(0, rig.upload_count);
    event_queue_tick(&rig.queue, EVENT_QUEUE_RETRY_MS);
    CHECK_EQ_INT(1, rig.upload_count);
}

static void test_events_queued_while_offline_all_go_out_in_order_with_their_times(void)
{
    Rig rig;
    uint32_t seq;
    int i;

    start(&rig);
    rig.online = false;
    for (seq = 1u; seq <= 5u; seq++) {
        push(&rig, seq, BRIDGE_OPENED_EMPTY);
        rig.clock_s += 60;   /* a Lid cycle a minute */
        run_loop(&rig, 100);   /* a second of main loop, offline */
    }
    CHECK_EQ_INT(0, rig.upload_count);
    CHECK_EQ_INT(5, event_queue_count(&rig.queue));

    rig.online = true;
    rig.now_ms += EVENT_QUEUE_RETRY_MS;
    run_loop(&rig, 5);
    CHECK_EQ_INT(5, rig.upload_count);
    for (i = 0; i < 5; i++) {
        CHECK_EQ_INT(i + 1, rig.uploads[i].event.seq);
        CHECK(rig.uploads[i].time.unix_s == SYNCED_S + 60 * i);
    }
    CHECK_EQ_INT(0, event_queue_count(&rig.queue));
}

static void test_a_full_queue_drops_the_oldest_event_and_reports_it(void)
{
    Rig rig;
    uint32_t seq;
    uint32_t total = EVENT_QUEUE_CAPACITY + 2u;

    start(&rig);
    rig.online = false;
    for (seq = 1u; seq <= total; seq++) {
        push(&rig, seq, BRIDGE_DELIVERED);
    }
    CHECK_EQ_INT(2, rig.dropped_count);
    CHECK_EQ_INT(1, rig.dropped[0]);
    CHECK_EQ_INT(2, rig.dropped[1]);
    CHECK_EQ_INT(EVENT_QUEUE_CAPACITY, event_queue_count(&rig.queue));

    rig.online = true;
    run_loop(&rig, (int)total);
    CHECK_EQ_INT(EVENT_QUEUE_CAPACITY, rig.upload_count);
    CHECK_EQ_INT(3, rig.uploads[0].event.seq);   /* the newest are kept */
    CHECK_EQ_INT(total, rig.uploads[EVENT_QUEUE_CAPACITY - 1u].event.seq);
    for (seq = 1u; seq < EVENT_QUEUE_CAPACITY; seq++) {
        CHECK(rig.uploads[seq].event.seq == rig.uploads[seq - 1u].event.seq + 1u);
    }
}

static void test_a_queue_that_fills_while_waiting_to_retry_drops_the_oldest_and_keeps_order(void)
{
    uint32_t seq;
    uint32_t total = EVENT_QUEUE_CAPACITY + 2u;
    Rig rig;

    start(&rig);
    push(&rig, 1u, BRIDGE_DELIVERED);
    rig.online = false;
    rig.now_ms = 1000u;
    event_queue_tick(&rig.queue, rig.now_ms);   /* fails: waiting to retry */
    for (seq = 2u; seq <= total; seq++) {
        push(&rig, seq, BRIDGE_COLLECTED);
        event_queue_tick(&rig.queue, rig.now_ms + seq);   /* still waiting */
    }
    CHECK_EQ_INT(1, rig.attempt_count);
    CHECK_EQ_INT(2, rig.dropped_count);
    CHECK_EQ_INT(1, rig.dropped[0]);
    CHECK_EQ_INT(2, rig.dropped[1]);

    rig.online = true;
    event_queue_tick(&rig.queue, rig.now_ms + EVENT_QUEUE_RETRY_MS - 1u);
    CHECK_EQ_INT(0, rig.upload_count);
    rig.now_ms += EVENT_QUEUE_RETRY_MS;   /* the first retry is due now */
    run_loop(&rig, (int)total);
    CHECK_EQ_INT(EVENT_QUEUE_CAPACITY, rig.upload_count);
    CHECK_EQ_INT(3, rig.uploads[0].event.seq);
    for (seq = 1u; seq < EVENT_QUEUE_CAPACITY; seq++) {
        CHECK(rig.uploads[seq].event.seq == rig.uploads[seq - 1u].event.seq + 1u);
    }
    CHECK_EQ_INT(0, event_queue_count(&rig.queue));
}

static void test_retry_now_ends_the_wait_so_the_queue_drains_at_once(void)
{
    Rig rig;
    start(&rig);
    push(&rig, 1u, BRIDGE_DELIVERED);
    push(&rig, 2u, BRIDGE_COLLECTED);
    rig.online = false;
    rig.now_ms = 1000u;
    event_queue_tick(&rig.queue, rig.now_ms);   /* fails */

    rig.online = true;   /* WiFi is back */
    event_queue_retry_now(&rig.queue);
    rig.now_ms += 10u;
    run_loop(&rig, 2);
    CHECK_EQ_INT(2, rig.upload_count);
    CHECK_EQ_INT(1, rig.uploads[0].event.seq);
    CHECK_EQ_INT(2, rig.uploads[1].event.seq);
}

static void test_retry_now_with_no_wait_running_changes_nothing(void)
{
    Rig rig;
    start(&rig);
    event_queue_retry_now(&rig.queue);   /* empty queue */
    run_loop(&rig, 1);
    CHECK_EQ_INT(0, rig.attempt_count);
    push(&rig, 1u, BRIDGE_DELIVERED);
    event_queue_retry_now(&rig.queue);
    run_loop(&rig, 1);
    CHECK_EQ_INT(1, rig.upload_count);
}

static void test_with_no_clock_every_event_is_stamped_unknown(void)
{
    Rig rig;
    EventQueueOutputs outputs;

    start(&rig);
    memset(&outputs, 0, sizeof outputs);
    outputs.context = &rig;
    outputs.upload = upload;   /* no clock_s */
    event_queue_init(&rig.queue, outputs);
    push(&rig, 1u, BRIDGE_DELIVERED);
    push(&rig, 2u, BRIDGE_COLLECTED);
    run_loop(&rig, 2);
    CHECK_EQ_INT(2, rig.upload_count);
    CHECK(!rig.uploads[0].time.known);
    CHECK(!rig.uploads[1].time.known);
}

static void test_a_full_queue_with_no_on_dropped_still_drops_the_oldest(void)
{
    Rig rig;
    EventQueueOutputs outputs;
    uint32_t seq;

    start(&rig);
    memset(&outputs, 0, sizeof outputs);
    outputs.context = &rig;
    outputs.clock_s = read_clock;
    outputs.upload = upload;   /* no on_dropped */
    event_queue_init(&rig.queue, outputs);
    for (seq = 1u; seq <= EVENT_QUEUE_CAPACITY + 1u; seq++) {
        push(&rig, seq, BRIDGE_DELIVERED);
    }
    CHECK_EQ_INT(EVENT_QUEUE_CAPACITY, event_queue_count(&rig.queue));
    run_loop(&rig, 1);
    CHECK_EQ_INT(2, rig.uploads[0].event.seq);
}

/* --- With the board link ------------------------------------------------ */

typedef struct {
    Rig rig;
    BridgeLink link;
    char replies[256];
} LinkRig;

static void link_reply(void *context, const char *line)
{
    LinkRig *both = context;
    strncat(both->replies, line, sizeof both->replies - strlen(both->replies) - 1u);
}

static void link_event(void *context, BridgeEvent event)
{
    LinkRig *both = context;
    event_queue_push(&both->rig.queue, event);
}

static void test_the_esp32_keeps_acking_while_offline_and_uploads_the_events_later(void)
{
    static const char lines[] =
        "BOOT*B6\nEVT,1,DELIVERED*4E\nEVT,2,COLLECTED*74\nEVT,2,COLLECTED*74\n";
    LinkRig both;
    BridgeLinkOutputs outputs;
    size_t i;

    start(&both.rig);
    both.replies[0] = '\0';
    memset(&outputs, 0, sizeof outputs);
    outputs.context = &both;
    outputs.write_line = link_reply;
    outputs.on_event = link_event;
    bridge_link_init(&both.link, outputs);

    both.rig.online = false;
    for (i = 0; lines[i] != '\0'; i++) {
        bridge_link_feed(&both.link, (uint8_t)lines[i]);
        run_loop(&both.rig, 1);
    }
    CHECK(strcmp(both.replies, "ACK,0*73\nACK,1*6E\nACK,2*49\nACK,2*49\n") == 0);
    CHECK_EQ_INT(0, both.rig.upload_count);

    both.rig.online = true;
    both.rig.now_ms += EVENT_QUEUE_RETRY_MS;
    run_loop(&both.rig, 3);
    CHECK_EQ_INT(2, both.rig.upload_count);   /* the resent EVT,2 once */
    CHECK_EQ_INT(1, both.rig.uploads[0].event.seq);
    CHECK_EQ_INT(2, both.rig.uploads[1].event.seq);
}

int main(void)
{
    RUN_TEST(test_an_event_is_uploaded_with_the_time_it_arrived);
    RUN_TEST(test_an_event_that_arrives_before_ntp_syncs_is_uploaded_as_unknown);
    RUN_TEST(test_an_empty_queue_uploads_nothing);
    RUN_TEST(test_events_are_uploaded_in_order_one_per_tick);
    RUN_TEST(test_a_failed_upload_keeps_the_event_and_waits_before_retrying);
    RUN_TEST(test_the_retry_wait_works_across_the_millisecond_clock_wrapping);
    RUN_TEST(test_events_queued_while_offline_all_go_out_in_order_with_their_times);
    RUN_TEST(test_a_full_queue_drops_the_oldest_event_and_reports_it);
    RUN_TEST(test_a_queue_that_fills_while_waiting_to_retry_drops_the_oldest_and_keeps_order);
    RUN_TEST(test_retry_now_ends_the_wait_so_the_queue_drains_at_once);
    RUN_TEST(test_retry_now_with_no_wait_running_changes_nothing);
    RUN_TEST(test_with_no_clock_every_event_is_stamped_unknown);
    RUN_TEST(test_a_full_queue_with_no_on_dropped_still_drops_the_oldest);
    RUN_TEST(test_the_esp32_keeps_acking_while_offline_and_uploads_the_events_later);
    return TEST_SUMMARY();
}
