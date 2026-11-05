/*
 * PC tests for the Box logic core. Each test feeds a sensor timeline and
 * ticks, then checks only what the core emitted.
 */
#include <string.h>

#include "box_core.h"
#include "test_harness.h"

#define TICK_MS 10u
#define MAX_RECORDED 32
#define MAX_BUZZER_RECORDED 256

/* ---- Recording what the core emits ------------------------------------- */

typedef struct {
    BoxEvent events[MAX_RECORDED];
    uint32_t event_times_ms[MAX_RECORDED];
    int event_count;
    BoxStatus statuses[MAX_RECORDED];
    int status_count;
    bool buzzer_on[MAX_BUZZER_RECORDED];
    uint32_t buzzer_times_ms[MAX_BUZZER_RECORDED];
    int buzzer_count;
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

static void record_box_status(void *context, BoxStatus status, uint32_t now_ms)
{
    Recorder *r = context;
    (void)now_ms;
    if (r->status_count < MAX_RECORDED) {
        r->statuses[r->status_count] = status;
    }
    r->status_count++;
}

static void record_buzzer(void *context, bool on, uint32_t now_ms)
{
    Recorder *r = context;
    if (r->buzzer_count < MAX_BUZZER_RECORDED) {
        r->buzzer_on[r->buzzer_count] = on;
        r->buzzer_times_ms[r->buzzer_count] = now_ms;
    }
    r->buzzer_count++;
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

/* Switch the Box on at `start_ms` with these first readings, and nothing
 * more. */
static void boot_at(Rig *rig, BoxReadings first, uint32_t start_ms)
{
    BoxCoreOutputs outputs = {0};
    memset(rig, 0, sizeof *rig);
    rig->now_ms = start_ms;
    outputs.context = &rig->recorder;
    outputs.on_event = record_event;
    outputs.on_box_status_changed = record_box_status;
    outputs.on_buzzer = record_buzzer;
    box_core_init(&rig->core, outputs, rig->now_ms, first);
}

/* Switch the Box on at time 0. */
static void boot(Rig *rig, BoxReadings first)
{
    boot_at(rig, first, 0);
}

/* Hold these readings for `duration_ms`, ticking every TICK_MS. Counts the
 * time elapsed rather than comparing clock values, so it keeps going when
 * now_ms wraps around. */
static void hold(Rig *rig, BoxReadings r, uint32_t duration_ms)
{
    uint32_t elapsed_ms;
    for (elapsed_ms = 0; elapsed_ms < duration_ms; elapsed_ms += TICK_MS) {
        rig->now_ms += TICK_MS;
        box_core_tick(&rig->core, rig->now_ms, r);
    }
}

/* Boot, then wait until the Box has read its starting state (one settle
 * with the Lid closed), so the test starts from a known Empty or Occupied
 * Box. `first` must have the Lid closed. */
static void power_up(Rig *rig, BoxReadings first)
{
    boot(rig, first);
    hold(rig, first, BOX_SETTLE_TIME_MS);
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

/* Compile-time check: the debounce boundary tests assume the debounce is a
 * whole number of ticks. */
typedef char debounce_must_be_whole_ticks[
    (BOX_LID_DEBOUNCE_MS % TICK_MS == 0) ? 1 : -1];

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

/* ---- Power-up ----------------------------------------------------------- */

static void test_powering_up_empty_emits_no_event(void)
{
    Rig rig;
    boot(&rig, readings(false, false));
    hold(&rig, readings(false, false), 5000);

    CHECK_EQ_INT(0, rig.recorder.event_count);
}

static void test_powering_up_occupied_emits_no_event(void)
{
    Rig rig;
    boot(&rig, readings(false, true));
    hold(&rig, readings(false, true), 5000);

    CHECK_EQ_INT(0, rig.recorder.event_count);
}

/* Exactly one Box status was reported, and it was this one. */
#define CHECK_ONLY_STATUS(rig, is_occupied, is_lid_open)                  \
    do {                                                                  \
        CHECK_EQ_INT(1, (rig).recorder.status_count);                     \
        if ((rig).recorder.status_count >= 1) {                           \
            CHECK_STATUS(rig, 0, is_occupied, is_lid_open);               \
        }                                                                 \
    } while (0)

/* The Box status reported in position `i` was this one. */
#define CHECK_STATUS(rig, i, is_occupied, is_lid_open)                    \
    do {                                                                  \
        CHECK_EQ_INT((is_occupied), (rig).recorder.statuses[i].occupied); \
        CHECK_EQ_INT((is_lid_open), (rig).recorder.statuses[i].lid_open); \
    } while (0)

/* The starting state is read once the Lid has been closed for a full settle
 * after boot, and reported as the first Box status. */
static void test_powering_up_reports_the_starting_box_status_after_one_settle(void)
{
    Rig rig;
    boot(&rig, readings(false, true));

    hold(&rig, readings(false, true), BOX_SETTLE_TIME_MS - TICK_MS);
    CHECK_EQ_INT(0, rig.recorder.status_count);

    hold(&rig, readings(false, true), TICK_MS);
    CHECK_ONLY_STATUS(rig, true, false);

    hold(&rig, readings(false, true), 5000);
    CHECK_ONLY_STATUS(rig, true, false);
    CHECK_EQ_INT(0, rig.recorder.event_count);
}

/* (a) The first raw sample at boot is a bounce: the Lid reads open for a
 * moment, then chatters and settles closed. No Lid cycle, no Event, and the
 * open reading is never reported. */
static void test_a_bouncing_first_lid_reading_at_boot_is_not_a_lid_cycle(void)
{
    Rig rig;
    boot(&rig, readings(true, false));
    reed_bounce(&rig, false, false);
    hold(&rig, readings(false, false), 5000);

    CHECK_EQ_INT(0, rig.recorder.event_count);
    CHECK_ONLY_STATUS(rig, false, false);
}

/* (b) Powered up with the Lid open and a hand in the beam. The starting
 * state isn't read with the Lid open: no Box status at all is reported while
 * it stays open, nor during the settle after it closes. Then the starting
 * state comes from the settled sample, with no Event. */
static void test_powering_up_with_the_lid_open_reports_nothing_until_it_closes_and_settles(void)
{
    Rig rig;
    boot(&rig, readings(true, true));
    hold(&rig, readings(true, true), 10000);
    CHECK_EQ_INT(0, rig.recorder.status_count);

    /* The first closed tick is TICK_MS from now, so holding for exactly the
     * settle time ends one tick short of it. */
    hold(&rig, readings(false, false), BOX_SETTLE_TIME_MS);
    CHECK_EQ_INT(0, rig.recorder.status_count);

    hold(&rig, readings(false, false), TICK_MS);
    CHECK_ONLY_STATUS(rig, false, false);

    hold(&rig, readings(false, false), 5000);
    CHECK_ONLY_STATUS(rig, false, false);
    CHECK_EQ_INT(0, rig.recorder.event_count);
}

/* A Lid cycle that starts before the power-up settle ends is part of power-up:
 * its "before" state is unknown, so it emits no Event, and the starting
 * state is read when it settles. */
static void test_a_lid_cycle_during_the_power_up_settle_emits_no_event(void)
{
    Rig rig;
    boot(&rig, readings(false, false));
    hold(&rig, readings(false, false), 500);

    lid_cycle(&rig, true);

    CHECK_EQ_INT(0, rig.recorder.event_count);
    CHECK_ONLY_STATUS(rig, true, false);
}

/* ---- Box status changes ------------------------------------------------- */

static void test_a_lid_cycle_that_changes_nothing_reports_lid_open_then_lid_closed(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));     /* status 0: Empty, closed */

    lid_cycle(&rig, false);

    CHECK_EQ_INT(3, rig.recorder.status_count);
    if (rig.recorder.status_count == 3) {
        CHECK_STATUS(rig, 0, false, false);
        CHECK_STATUS(rig, 1, false, true);
        CHECK_STATUS(rig, 2, false, false);
    }
}

static void test_a_delivery_also_reports_the_change_to_occupied(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));     /* status 0: Empty, closed */

    lid_cycle(&rig, true);

    CHECK_EQ_INT(4, rig.recorder.status_count);
    if (rig.recorder.status_count == 4) {
        CHECK_STATUS(rig, 0, false, false);     /* starting state: Empty */
        CHECK_STATUS(rig, 1, false, true);      /* Lid open */
        CHECK_STATUS(rig, 2, false, false);     /* Lid closed, settling */
        CHECK_STATUS(rig, 3, true, false);      /* settled: Occupied */
    }
    CHECK_ONLY_EVENT(rig, BOX_EVENT_DELIVERED);
}

static void test_no_box_status_is_reported_when_nothing_changes(void)
{
    Rig rig;
    power_up(&rig, readings(false, true));      /* status 0: Occupied, closed */

    /* Raw Lid glitches too short to count are not reported either: the Box
     * status shows the debounced Lid. */
    hold(&rig, readings(false, true), 3000);
    hold(&rig, readings(true, true), LONGEST_IGNORED_GLITCH_MS);
    hold(&rig, readings(false, true), 3000);
    reed_bounce(&rig, false, true);
    hold(&rig, readings(false, true), 3000);

    CHECK_ONLY_STATUS(rig, true, false);
    CHECK_EQ_INT(0, rig.recorder.event_count);
}

/* ---- Buzzer ------------------------------------------------------------- */

/* Buzzer command `i` was `is_on`, sent at `at_ms`. */
#define CHECK_BUZZER(rig, i, is_on, at_ms)                                \
    do {                                                                  \
        CHECK_EQ_INT((is_on), (rig).recorder.buzzer_on[i]);               \
        CHECK_EQ_INT((at_ms), (rig).recorder.buzzer_times_ms[i]);         \
    } while (0)

static void test_a_delivery_is_one_short_beep(void)
{
    Rig rig;
    uint32_t delivered_at_ms;
    power_up(&rig, readings(false, false));

    lid_cycle(&rig, true);
    CHECK_ONLY_EVENT(rig, BOX_EVENT_DELIVERED);
    delivered_at_ms = rig.recorder.event_times_ms[0];

    hold(&rig, readings(false, true), 10000);
    CHECK_EQ_INT(2, rig.recorder.buzzer_count);
    if (rig.recorder.buzzer_count == 2) {
        CHECK_BUZZER(rig, 0, true, delivered_at_ms);
        CHECK_BUZZER(rig, 1, false, delivered_at_ms + BOX_DELIVERY_BEEP_MS);
    }
}

static void test_collections_and_opened_lid_cycles_do_not_beep(void)
{
    Rig rig;
    power_up(&rig, readings(false, true));

    lid_cycle(&rig, true);      /* Opened while occupied */
    lid_cycle(&rig, false);     /* Collection */
    lid_cycle(&rig, false);     /* Opened while empty */
    hold(&rig, readings(false, false), 5000);

    CHECK_EQ_INT(3, rig.recorder.event_count);
    CHECK_EQ_INT(0, rig.recorder.buzzer_count);
}

/* hold() ticks at the end of each step, so the Lid first reads open TICK_MS
 * after the hold starts: holding it open for exactly BOX_LID_LEFT_OPEN_MS
 * ends one tick short of the threshold. */
static void test_the_nag_starts_once_the_lid_has_been_open_past_the_threshold(void)
{
    Rig rig;
    uint32_t opened_at_ms;
    power_up(&rig, readings(false, false));
    opened_at_ms = rig.now_ms + TICK_MS;

    hold(&rig, readings(true, false), BOX_LID_LEFT_OPEN_MS);
    CHECK_EQ_INT(0, rig.recorder.buzzer_count);

    hold(&rig, readings(true, false), TICK_MS);
    CHECK_EQ_INT(1, rig.recorder.buzzer_count);
    if (rig.recorder.buzzer_count == 1) {
        CHECK_BUZZER(rig, 0, true, opened_at_ms + BOX_LID_LEFT_OPEN_MS);
    }
}

#define NAG_PERIOD_MS (BOX_NAG_BEEP_ON_MS + BOX_NAG_BEEP_OFF_MS)

/* Compile-time check: the buzzer timing tests expect each beep to start and
 * end exactly on a tick. */
typedef char buzzer_times_must_be_whole_ticks[
    (BOX_DELIVERY_BEEP_MS % TICK_MS == 0 && BOX_LID_LEFT_OPEN_MS % TICK_MS == 0 &&
     BOX_NAG_BEEP_ON_MS % TICK_MS == 0 && BOX_NAG_BEEP_OFF_MS % TICK_MS == 0) ? 1 : -1];

static void test_the_nag_repeats_while_the_lid_stays_open(void)
{
    Rig rig;
    uint32_t nag_at_ms;
    int beep;
    power_up(&rig, readings(false, false));
    nag_at_ms = rig.now_ms + TICK_MS + BOX_LID_LEFT_OPEN_MS;

    /* Three whole beeps: on, off, on, off, on, off. The fourth beep would
     * start one tick after the hold ends. */
    hold(&rig, readings(true, false), BOX_LID_LEFT_OPEN_MS + 3 * NAG_PERIOD_MS);

    CHECK_EQ_INT(6, rig.recorder.buzzer_count);
    if (rig.recorder.buzzer_count == 6) {
        for (beep = 0; beep < 3; beep++) {
            uint32_t start_ms = nag_at_ms + (uint32_t)beep * NAG_PERIOD_MS;
            CHECK_BUZZER(rig, 2 * beep, true, start_ms);
            CHECK_BUZZER(rig, 2 * beep + 1, false, start_ms + BOX_NAG_BEEP_ON_MS);
        }
    }
}

/* The nag stops on the first tick the reed switch reads closed, in the
 * middle of a beep, without waiting for the close to pass the debounce, and
 * never comes back while the Lid stays closed. */
static void test_closing_the_lid_stops_the_nag_at_once(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));
    hold(&rig, readings(true, false), BOX_LID_LEFT_OPEN_MS + TICK_MS);
    CHECK_EQ_INT(1, rig.recorder.buzzer_count);     /* the first nag beep */

    hold(&rig, readings(false, false), TICK_MS);    /* the reed closes */
    CHECK_EQ_INT(2, rig.recorder.buzzer_count);
    if (rig.recorder.buzzer_count == 2) {
        CHECK_BUZZER(rig, 1, false, rig.now_ms);
    }

    hold(&rig, readings(false, false), 60000);
    CHECK_EQ_INT(2, rig.recorder.buzzer_count);
}

/* The Lid closes exactly when the second nag beep would start, i.e. while
 * the close is still inside the debounce. No beep starts after the Lid is
 * shut, not even a short chirp. */
static void test_closing_the_lid_as_a_nag_beep_is_due_does_not_start_it(void)
{
    Rig rig;
    uint32_t nag_at_ms;
    power_up(&rig, readings(false, false));
    nag_at_ms = rig.now_ms + TICK_MS + BOX_LID_LEFT_OPEN_MS;

    /* The first closed tick is exactly one nag period after the nag began. */
    hold(&rig, readings(true, false), BOX_LID_LEFT_OPEN_MS + NAG_PERIOD_MS);
    hold(&rig, readings(false, false), 60000);

    CHECK_EQ_INT(2, rig.recorder.buzzer_count);     /* only the first beep */
    if (rig.recorder.buzzer_count == 2) {
        CHECK_BUZZER(rig, 0, true, nag_at_ms);
        CHECK_BUZZER(rig, 1, false, nag_at_ms + BOX_NAG_BEEP_ON_MS);
    }
}

/* Each opening of the Lid gets the full threshold again: time spent open
 * before an earlier close doesn't count. Held open for exactly the
 * threshold, the Lid reads closed on the tick the nag would start. */
static void test_a_lid_closed_again_before_the_threshold_never_nags(void)
{
    Rig rig;
    power_up(&rig, readings(false, false));

    hold(&rig, readings(true, false), BOX_LID_LEFT_OPEN_MS);
    hold(&rig, readings(false, false), 5000);
    hold(&rig, readings(true, false), BOX_LID_LEFT_OPEN_MS);
    hold(&rig, readings(false, false), 60000);

    CHECK_EQ_INT(2, rig.recorder.event_count);      /* two Opened while empty */
    CHECK_EQ_INT(0, rig.recorder.buzzer_count);
}

/* The Courier leaves the Lid open long enough to be nagged, then closes it
 * with the parcel inside: the nag stops at the close, and the Delivery still
 * gets its one full beep after the settle. */
static void test_a_nag_before_a_delivery_does_not_change_the_delivery_beep(void)
{
    Rig rig;
    uint32_t delivered_at_ms;
    int n;
    power_up(&rig, readings(false, false));

    hold(&rig, readings(true, true), BOX_LID_LEFT_OPEN_MS + 2 * NAG_PERIOD_MS);
    hold(&rig, readings(false, true), 5000);
    hold(&rig, readings(false, true), 5000);
    CHECK_ONLY_EVENT(rig, BOX_EVENT_DELIVERED);
    delivered_at_ms = rig.recorder.event_times_ms[0];

    /* Nag beeps (on, off, ...) that all end by the close, then the Delivery
     * beep, whole. */
    n = rig.recorder.buzzer_count;
    CHECK(n >= 6 && n <= MAX_BUZZER_RECORDED && n % 2 == 0);
    if (n >= 6 && n <= MAX_BUZZER_RECORDED) {
        /* The settle runs from the real close, so that is when it was. The
         * nag's last command is off, at or before it. */
        CHECK_EQ_INT(false, rig.recorder.buzzer_on[n - 3]);
        CHECK(rig.recorder.buzzer_times_ms[n - 3] <=
              delivered_at_ms - BOX_SETTLE_TIME_MS);
        CHECK_BUZZER(rig, n - 2, true, delivered_at_ms);
        CHECK_BUZZER(rig, n - 1, false, delivered_at_ms + BOX_DELIVERY_BEEP_MS);
    }
}

/* The Lid opens again right as the Delivery beep starts: the beep still
 * lasts its full length, and the new opening doesn't nag before its own
 * threshold. */
static void test_reopening_the_lid_during_the_delivery_beep_does_not_cut_it_short(void)
{
    Rig rig;
    uint32_t delivered_at_ms;
    power_up(&rig, readings(false, false));
    hold(&rig, readings(true, false), 1000);

    /* The settle ends on the last tick of this hold. */
    hold(&rig, readings(false, true), BOX_SETTLE_TIME_MS + TICK_MS);
    CHECK_ONLY_EVENT(rig, BOX_EVENT_DELIVERED);
    delivered_at_ms = rig.now_ms;

    hold(&rig, readings(true, true), BOX_LID_LEFT_OPEN_MS);

    CHECK_EQ_INT(2, rig.recorder.buzzer_count);
    if (rig.recorder.buzzer_count == 2) {
        CHECK_BUZZER(rig, 0, true, delivered_at_ms);
        CHECK_BUZZER(rig, 1, false, delivered_at_ms + BOX_DELIVERY_BEEP_MS);
    }
}

/* Decision: a Lid open at boot is a Lid left open too, so the nag doesn't
 * wait for the starting state. Its time is counted from boot, the earliest
 * the Box can know the Lid was open. Closing it stops the nag as usual. */
static void test_a_lid_open_at_boot_nags_once_past_the_threshold(void)
{
    Rig rig;
    boot(&rig, readings(true, false));

    hold(&rig, readings(true, false), BOX_LID_LEFT_OPEN_MS - TICK_MS);
    CHECK_EQ_INT(0, rig.recorder.buzzer_count);

    hold(&rig, readings(true, false), TICK_MS);
    CHECK_EQ_INT(1, rig.recorder.buzzer_count);
    if (rig.recorder.buzzer_count == 1) {
        CHECK_BUZZER(rig, 0, true, BOX_LID_LEFT_OPEN_MS);
    }
    CHECK_EQ_INT(0, rig.recorder.status_count);     /* still no starting state */

    hold(&rig, readings(false, false), TICK_MS);    /* the reed closes */
    CHECK_EQ_INT(2, rig.recorder.buzzer_count);
    if (rig.recorder.buzzer_count == 2) {
        CHECK_BUZZER(rig, 1, false, rig.now_ms);
    }
    hold(&rig, readings(false, false), 60000);
    CHECK_EQ_INT(2, rig.recorder.buzzer_count);
}

/* HAL_GetTick() wraps to 0 after about 49.7 days. Boot so that the clock
 * wraps 100 ms into the Delivery beep: it still lasts its full length. */
static void test_the_delivery_beep_keeps_time_across_the_millisecond_wraparound(void)
{
    Rig rig;
    uint32_t delivered_at_ms;
    /* Settle, open 1000 ms, then the Delivery one tick past the next settle:
     * 5010 ms after boot, and the wrap is 100 ms later. */
    boot_at(&rig, readings(false, false), 0u - (5010u + 100u));
    hold(&rig, readings(false, false), BOX_SETTLE_TIME_MS);
    hold(&rig, readings(true, false), 1000);
    hold(&rig, readings(false, true), BOX_SETTLE_TIME_MS + TICK_MS);
    CHECK_ONLY_EVENT(rig, BOX_EVENT_DELIVERED);
    delivered_at_ms = rig.now_ms;

    hold(&rig, readings(false, true), 5000);

    CHECK_EQ_INT(2, rig.recorder.buzzer_count);
    if (rig.recorder.buzzer_count == 2) {
        CHECK_BUZZER(rig, 0, true, delivered_at_ms);
        CHECK_BUZZER(rig, 1, false, delivered_at_ms + BOX_DELIVERY_BEEP_MS);
        /* The beep really straddles the wrap. */
        CHECK(rig.recorder.buzzer_times_ms[1] < rig.recorder.buzzer_times_ms[0]);
    }
}

/* Boot so that the clock wraps during the second nag beep: the threshold
 * and every beep keep their times. */
static void test_the_nag_keeps_time_across_the_millisecond_wraparound(void)
{
    Rig rig;
    uint32_t nag_at_ms;
    int beep;
    /* The Lid opens one tick after the settle, so the nag starts
     * BOX_SETTLE_TIME_MS + TICK_MS + BOX_LID_LEFT_OPEN_MS after boot, and
     * the wrap comes 1100 ms after that, during the second beep. */
    boot_at(&rig, readings(false, false),
            0u - (BOX_SETTLE_TIME_MS + TICK_MS + BOX_LID_LEFT_OPEN_MS + 1100u));
    hold(&rig, readings(false, false), BOX_SETTLE_TIME_MS);
    nag_at_ms = rig.now_ms + TICK_MS + BOX_LID_LEFT_OPEN_MS;

    hold(&rig, readings(true, false), BOX_LID_LEFT_OPEN_MS + 3 * NAG_PERIOD_MS);

    CHECK_EQ_INT(6, rig.recorder.buzzer_count);
    if (rig.recorder.buzzer_count == 6) {
        for (beep = 0; beep < 3; beep++) {
            uint32_t start_ms = nag_at_ms + (uint32_t)beep * NAG_PERIOD_MS;
            CHECK_BUZZER(rig, 2 * beep, true, start_ms);
            CHECK_BUZZER(rig, 2 * beep + 1, false, start_ms + BOX_NAG_BEEP_ON_MS);
        }
        /* The second beep really straddles the wrap. */
        CHECK(rig.recorder.buzzer_times_ms[3] < rig.recorder.buzzer_times_ms[2]);
    }
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
    RUN_TEST(test_powering_up_empty_emits_no_event);
    RUN_TEST(test_powering_up_occupied_emits_no_event);
    RUN_TEST(test_powering_up_reports_the_starting_box_status_after_one_settle);
    RUN_TEST(test_a_bouncing_first_lid_reading_at_boot_is_not_a_lid_cycle);
    RUN_TEST(test_powering_up_with_the_lid_open_reports_nothing_until_it_closes_and_settles);
    RUN_TEST(test_a_lid_cycle_during_the_power_up_settle_emits_no_event);
    RUN_TEST(test_a_lid_cycle_that_changes_nothing_reports_lid_open_then_lid_closed);
    RUN_TEST(test_a_delivery_also_reports_the_change_to_occupied);
    RUN_TEST(test_no_box_status_is_reported_when_nothing_changes);
    RUN_TEST(test_a_delivery_is_one_short_beep);
    RUN_TEST(test_collections_and_opened_lid_cycles_do_not_beep);
    RUN_TEST(test_the_nag_starts_once_the_lid_has_been_open_past_the_threshold);
    RUN_TEST(test_the_nag_repeats_while_the_lid_stays_open);
    RUN_TEST(test_closing_the_lid_stops_the_nag_at_once);
    RUN_TEST(test_closing_the_lid_as_a_nag_beep_is_due_does_not_start_it);
    RUN_TEST(test_a_lid_closed_again_before_the_threshold_never_nags);
    RUN_TEST(test_a_nag_before_a_delivery_does_not_change_the_delivery_beep);
    RUN_TEST(test_reopening_the_lid_during_the_delivery_beep_does_not_cut_it_short);
    RUN_TEST(test_a_lid_open_at_boot_nags_once_past_the_threshold);
    RUN_TEST(test_the_delivery_beep_keeps_time_across_the_millisecond_wraparound);
    RUN_TEST(test_the_nag_keeps_time_across_the_millisecond_wraparound);
    return TEST_SUMMARY();
}
