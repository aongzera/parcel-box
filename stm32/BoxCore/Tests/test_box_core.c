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

/* One Lid cycle: open for a second, then closed long enough to settle,
 * with `parcel_present_after` showing whether a parcel is inside. */
static void lid_cycle(Rig *rig, bool parcel_present_after)
{
    hold(rig, readings(true, parcel_present_after), 1000);
    hold(rig, readings(false, parcel_present_after), 3000);
}

/* ---- Tests -------------------------------------------------------------- */

static void test_empty_box_lid_cycle_with_parcel_left_inside_is_one_delivery(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));
    hold(&rig, readings(false, false), 500);

    lid_cycle(&rig, true);     /* Courier leaves a parcel inside */

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

static void test_occupied_box_lid_cycle_with_parcel_taken_out_is_one_collection(void)
{
    Rig rig;
    power_up(&rig, readings(false, true));
    hold(&rig, readings(false, true), 500);

    lid_cycle(&rig, false);    /* Owner takes the parcel out */

    CHECK_ONLY_EVENT(rig, BOX_EVENT_COLLECTED);
}

static void test_occupied_box_lid_cycle_still_occupied_is_one_opened_occupied(void)
{
    Rig rig;
    power_up(&rig, readings(false, true));

    lid_cycle(&rig, true);     /* still a parcel inside */

    CHECK_ONLY_EVENT(rig, BOX_EVENT_OPENED_OCCUPIED);
}

static void test_empty_box_lid_cycle_still_empty_is_one_opened_empty(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));

    lid_cycle(&rig, false);    /* someone looks inside, nothing changes */

    CHECK_ONLY_EVENT(rig, BOX_EVENT_OPENED_EMPTY);
}

static void test_several_lid_cycles_in_a_row_track_the_box_state(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));

    lid_cycle(&rig, true);     /* Courier drops a parcel in */
    lid_cycle(&rig, true);     /* opened again, parcel still there */
    lid_cycle(&rig, false);    /* Owner takes it out */
    lid_cycle(&rig, false);    /* opened again, still empty */

    CHECK_EQ_INT(4, rig.recorder.event_count);
    if (rig.recorder.event_count == 4) {
        CHECK_EQ_INT(BOX_EVENT_DELIVERED, rig.recorder.events[0]);
        CHECK_EQ_INT(BOX_EVENT_OPENED_OCCUPIED, rig.recorder.events[1]);
        CHECK_EQ_INT(BOX_EVENT_COLLECTED, rig.recorder.events[2]);
        CHECK_EQ_INT(BOX_EVENT_OPENED_EMPTY, rig.recorder.events[3]);
    }
}

static void test_a_hand_in_the_box_while_the_lid_is_open_is_not_a_parcel(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));

    /* A Courier's hand is in the IR beam the whole time the Lid is open,
     * but nothing is left behind. */
    hold(&rig, readings(true, true), 3000);
    hold(&rig, readings(false, false), 5000);

    CHECK_ONLY_EVENT(rig, BOX_EVENT_OPENED_EMPTY);
}

/* Decision: reopening the Lid during the settle means the Lid cycle isn't
 * over yet. The settle is cancelled and restarts on the next close, and the
 * cycle still compares against the state before the FIRST open. So one
 * open-close-reopen-close sequence is one Lid cycle and one Event, judged
 * only once the Lid has finally stayed closed (product.md section 4). */
static void test_reopening_the_lid_during_the_settle_restarts_it_and_is_one_lid_cycle(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));

    hold(&rig, readings(true, false), 1000);    /* Courier opens the Lid */
    hold(&rig, readings(false, true), 1000);    /* closes it, half a settle */
    hold(&rig, readings(true, true), 1000);     /* reopens to push it in */

    /* Holding for the settle time ends one tick short of a full settle
     * after the second close, so nothing is decided yet. */
    hold(&rig, readings(false, true), BOX_SETTLE_TIME_MS);
    CHECK_EQ_INT(0, rig.recorder.event_count);

    hold(&rig, readings(false, true), TICK_MS);
    CHECK_ONLY_EVENT(rig, BOX_EVENT_DELIVERED);
}

static void test_a_one_tick_lid_glitch_while_closed_is_not_a_lid_cycle(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));
    hold(&rig, readings(false, false), 1000);

    hold(&rig, readings(true, false), TICK_MS); /* reed switch blips open */
    hold(&rig, readings(false, false), 5000);

    CHECK_EQ_INT(0, rig.recorder.event_count);
}

/* The longest Lid change the debounce must still ignore. hold() ticks at the
 * end of each step, so a change held for N ms is only N - TICK_MS old on its
 * last tick: a hold of exactly BOX_LID_DEBOUNCE_MS is still too short to
 * count, and one tick more is the shortest that does. */
#define LONGEST_IGNORED_GLITCH_MS BOX_LID_DEBOUNCE_MS
#define SHORTEST_COUNTED_LID_CHANGE_MS (BOX_LID_DEBOUNCE_MS + TICK_MS)

/* Compile-time check: the glitch-during-the-settle test fits its glitch
 * into the second half of the settle. */
typedef char debounce_must_fit_inside_the_settle[
    (BOX_LID_DEBOUNCE_MS < BOX_SETTLE_TIME_MS - 1000u) ? 1 : -1];

static void test_a_lid_glitch_just_shorter_than_the_debounce_is_not_a_lid_cycle(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));
    hold(&rig, readings(false, false), 1000);

    hold(&rig, readings(true, false), LONGEST_IGNORED_GLITCH_MS);
    hold(&rig, readings(false, false), 5000);

    CHECK_EQ_INT(0, rig.recorder.event_count);
}

/* The reed switch chattering as the Lid moves: four one-tick flips, ending
 * on the reading opposite to `settles_lid_open`, which the caller then
 * holds. Each flip is far shorter than BOX_LID_DEBOUNCE_MS. */
static void reed_bounce(Rig *rig, bool settles_lid_open, bool parcel_present)
{
    int flip;
    for (flip = 0; flip < 4; flip++) {
        bool lid_open = (flip % 2 == 0) ? settles_lid_open : !settles_lid_open;
        hold(rig, readings(lid_open, parcel_present), TICK_MS);
    }
}

static void test_reed_switch_bounce_on_open_and_close_is_one_lid_cycle(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));

    reed_bounce(&rig, true, false);
    hold(&rig, readings(true, false), 1000);

    reed_bounce(&rig, false, true);
    /* The settle runs from the last bounce, when the Lid really closed:
     * holding for the settle time ends one tick short of it. */
    hold(&rig, readings(false, true), BOX_SETTLE_TIME_MS);
    CHECK_EQ_INT(0, rig.recorder.event_count);

    hold(&rig, readings(false, true), TICK_MS);
    CHECK_ONLY_EVENT(rig, BOX_EVENT_DELIVERED);
}

static void test_a_lid_glitch_during_the_settle_does_not_restart_it(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));
    hold(&rig, readings(true, false), 1000);

    /* The Lid closes, then the reed switch blips open, just too briefly to
     * count, halfway through the settle. The settle still ends on time. */
    hold(&rig, readings(false, true), 1000);
    hold(&rig, readings(true, true), LONGEST_IGNORED_GLITCH_MS);
    hold(&rig, readings(false, true),
         BOX_SETTLE_TIME_MS - 1000 - LONGEST_IGNORED_GLITCH_MS);
    CHECK_EQ_INT(0, rig.recorder.event_count);

    hold(&rig, readings(false, true), TICK_MS);
    CHECK_ONLY_EVENT(rig, BOX_EVENT_DELIVERED);
}

static void test_a_lid_open_just_longer_than_the_debounce_is_a_lid_cycle(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));
    hold(&rig, readings(false, false), 1000);

    hold(&rig, readings(true, false), SHORTEST_COUNTED_LID_CHANGE_MS);
    hold(&rig, readings(false, false), 5000);

    CHECK_ONLY_EVENT(rig, BOX_EVENT_OPENED_EMPTY);
}

/* The Lid reopens two ticks before the settle would end. The reopen hasn't
 * passed the debounce yet when the settle time runs out, but the settle must
 * not finish with a hand in the beam: it is the same Lid cycle, judged once
 * the Lid finally stays closed (product.md section 4). */
static void test_reopening_the_lid_just_before_the_settle_ends_is_still_one_lid_cycle(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));
    hold(&rig, readings(true, false), 1000);

    hold(&rig, readings(false, false), BOX_SETTLE_TIME_MS - 2 * TICK_MS);
    hold(&rig, readings(true, true), 1000);     /* reopened, hand inside */
    hold(&rig, readings(false, false), 5000);   /* nothing left behind */

    CHECK_ONLY_EVENT(rig, BOX_EVENT_OPENED_EMPTY);
}

/* A glitch too short to count, right at the end of the settle, only delays
 * the end of the settle until the reading is closed again. */
static void test_a_lid_glitch_at_the_end_of_the_settle_delays_it_and_is_one_lid_cycle(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));
    hold(&rig, readings(true, false), 1000);

    hold(&rig, readings(false, false), BOX_SETTLE_TIME_MS - 2 * TICK_MS);
    hold(&rig, readings(true, true), LONGEST_IGNORED_GLITCH_MS);
    CHECK_EQ_INT(0, rig.recorder.event_count);

    hold(&rig, readings(false, false), 5000);
    CHECK_ONLY_EVENT(rig, BOX_EVENT_OPENED_EMPTY);
}

/* The Lid reopens one tick before the settle ends, and the reed switch
 * bounces as it does: open, closed, open, closed, then open for good. A
 * one-tick closed reading in that bounce is not the Lid being closed, so the
 * settle must not finish on it with a hand in the beam. */
static void test_a_bouncing_reopen_as_the_settle_ends_is_still_one_lid_cycle(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));
    hold(&rig, readings(true, false), 1000);

    hold(&rig, readings(false, false), BOX_SETTLE_TIME_MS - TICK_MS);
    reed_bounce(&rig, true, true);              /* reopening, hand inside */
    hold(&rig, readings(true, true), 1000);
    hold(&rig, readings(false, false), 5000);   /* nothing left behind */

    CHECK_ONLY_EVENT(rig, BOX_EVENT_OPENED_EMPTY);
}

/* The longest closed blip the debounce ignores, in the middle of a reopen
 * as the settle ends, still isn't a close: the settle doesn't finish on it. */
static void test_a_closed_blip_within_a_reopen_as_the_settle_ends_is_not_a_close(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));
    hold(&rig, readings(true, false), 1000);

    hold(&rig, readings(false, false), BOX_SETTLE_TIME_MS - TICK_MS);
    hold(&rig, readings(true, true), TICK_MS);                    /* reopening */
    hold(&rig, readings(false, true), LONGEST_IGNORED_GLITCH_MS); /* blip */
    hold(&rig, readings(true, true), 1000);                       /* hand inside */
    hold(&rig, readings(false, false), 5000);                     /* left empty */

    CHECK_ONLY_EVENT(rig, BOX_EVENT_OPENED_EMPTY);
}

int main(void)
{
    RUN_TEST(test_empty_box_lid_cycle_with_parcel_left_inside_is_one_delivery);
    RUN_TEST(test_delivery_waits_the_settle_time_after_the_lid_closes);
    RUN_TEST(test_parcel_presence_is_sampled_after_the_settle_not_at_the_close);
    RUN_TEST(test_occupied_box_lid_cycle_with_parcel_taken_out_is_one_collection);
    RUN_TEST(test_occupied_box_lid_cycle_still_occupied_is_one_opened_occupied);
    RUN_TEST(test_empty_box_lid_cycle_still_empty_is_one_opened_empty);
    RUN_TEST(test_several_lid_cycles_in_a_row_track_the_box_state);
    RUN_TEST(test_a_hand_in_the_box_while_the_lid_is_open_is_not_a_parcel);
    RUN_TEST(test_reopening_the_lid_during_the_settle_restarts_it_and_is_one_lid_cycle);
    RUN_TEST(test_a_one_tick_lid_glitch_while_closed_is_not_a_lid_cycle);
    RUN_TEST(test_a_lid_glitch_just_shorter_than_the_debounce_is_not_a_lid_cycle);
    RUN_TEST(test_reed_switch_bounce_on_open_and_close_is_one_lid_cycle);
    RUN_TEST(test_a_lid_glitch_during_the_settle_does_not_restart_it);
    RUN_TEST(test_a_lid_open_just_longer_than_the_debounce_is_a_lid_cycle);
    RUN_TEST(test_reopening_the_lid_just_before_the_settle_ends_is_still_one_lid_cycle);
    RUN_TEST(test_a_lid_glitch_at_the_end_of_the_settle_delays_it_and_is_one_lid_cycle);
    RUN_TEST(test_a_bouncing_reopen_as_the_settle_ends_is_still_one_lid_cycle);
    RUN_TEST(test_a_closed_blip_within_a_reopen_as_the_settle_ends_is_not_a_close);
    return TEST_SUMMARY();
}
