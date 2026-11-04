/*
 * PC tests for the Box logic core. Each test feeds a sensor timeline and
 * ticks, then checks only what the core emitted.
 */
#include <string.h>

#include "box_core.h"
#include "test_harness.h"

#define TICK_MS 10u
#define MAX_RECORDED 32

/* ---- Recording what the core emits ------------------------------------- */

typedef struct {
    BoxEvent events[MAX_RECORDED];
    uint32_t event_times_ms[MAX_RECORDED];
    int event_count;
} Recorder;

static void record_event(void *context, BoxEvent event, uint32_t now_ms)
{
    Recorder *r = context;
    if (r->event_count < MAX_RECORDED) {
        r->events[r->event_count] = event;
        r->event_times_ms[r->event_count] = now_ms;
    }
    r->event_count++;
}

/* ---- Driving a sensor timeline ------------------------------------------ */

typedef struct {
    BoxCore core;
    Recorder recorder;
    uint32_t now_ms;
} Rig;

static BoxReadings readings(bool lid_open, bool parcel_present)
{
    BoxReadings r;
    r.lid_open = lid_open;
    r.parcel_present = parcel_present;
    return r;
}

static void power_up(Rig *rig, BoxReadings first)
{
    BoxCoreOutputs outputs = {0};
    memset(rig, 0, sizeof *rig);
    outputs.context = &rig->recorder;
    outputs.on_event = record_event;
    box_core_init(&rig->core, outputs, rig->now_ms, first);
}

/* Hold these readings for `duration_ms`, ticking every TICK_MS. */
static void hold(Rig *rig, BoxReadings r, uint32_t duration_ms)
{
    uint32_t end_ms = rig->now_ms + duration_ms;
    while (rig->now_ms < end_ms) {
        rig->now_ms += TICK_MS;
        box_core_tick(&rig->core, rig->now_ms, r);
    }
}

/* Exactly one Event was emitted, and it was `expected`. events[0] is only
 * read when it was written. */
#define CHECK_ONLY_EVENT(rig, expected)                                   \
    do {                                                                  \
        CHECK_EQ_INT(1, (rig).recorder.event_count);                      \
        if ((rig).recorder.event_count >= 1) {                            \
            CHECK_EQ_INT((expected), (rig).recorder.events[0]);           \
        }                                                                 \
    } while (0)

/* ---- Tests -------------------------------------------------------------- */

static void test_empty_box_lid_cycle_with_parcel_left_inside_is_one_delivery(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));
    hold(&rig, readings(false, false), 500);

    hold(&rig, readings(true, false), 1000);    /* Courier opens the Lid */
    hold(&rig, readings(false, true), 5000);    /* closes it, parcel inside */

    CHECK_ONLY_EVENT(rig, BOX_EVENT_DELIVERED);
}

static void test_delivery_waits_the_settle_time_after_the_lid_closes(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));
    hold(&rig, readings(true, false), 1000);

    /* The first closed tick is TICK_MS from now, so holding for exactly the
     * settle time ends one tick short of it. */
    hold(&rig, readings(false, true), BOX_SETTLE_TIME_MS);
    CHECK_EQ_INT(0, rig.recorder.event_count);

    hold(&rig, readings(false, true), TICK_MS);
    CHECK_ONLY_EVENT(rig, BOX_EVENT_DELIVERED);
}

static void test_parcel_presence_is_sampled_after_the_settle_not_at_the_close(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));
    hold(&rig, readings(true, false), 1000);

    /* The parcel only comes into the IR beam partway through the settle. */
    hold(&rig, readings(false, false), 1000);
    hold(&rig, readings(false, true), 3000);

    CHECK_ONLY_EVENT(rig, BOX_EVENT_DELIVERED);
}

int main(void)
{
    RUN_TEST(test_empty_box_lid_cycle_with_parcel_left_inside_is_one_delivery);
    RUN_TEST(test_delivery_waits_the_settle_time_after_the_lid_closes);
    RUN_TEST(test_parcel_presence_is_sampled_after_the_settle_not_at_the_close);
    return TEST_SUMMARY();
}
