/*
 * PC tests for the UART line writer (box_lines). Each test feeds Box status
 * changes, Events and ticks, then checks only the lines it wrote.
 */
#include <string.h>

#include "box_lines.h"
#include "test_harness.h"

#define MAX_LINES 64
#define LINE_LEN 64

typedef struct {
    char lines[MAX_LINES][LINE_LEN];
    int count;
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

typedef struct {
    BoxLines lines;
    Recorder recorder;
} Rig;

static void start(Rig *rig)
{
    BoxLinesOutput output;
    memset(rig, 0, sizeof *rig);
    output.context = &rig->recorder;
    output.write_line = record_line;
    box_lines_init(&rig->lines, output);
}

static BoxStatus status(bool occupied, bool lid_open)
{
    BoxStatus s;
    s.occupied = occupied;
    s.lid_open = lid_open;
    return s;
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

static void test_a_box_status_change_writes_one_sts_line(void)
{
    Rig rig;
    start(&rig);

    box_lines_on_box_status_changed(&rig.lines, status(false, false), 2000);

    CHECK_EQ_INT(1, rig.recorder.count);
    CHECK_LINE(&rig, 0, "STS,EMPTY,LID_CLOSED\n");
}

static void test_sts_lines_name_every_box_status(void)
{
    Rig rig;
    start(&rig);

    box_lines_on_box_status_changed(&rig.lines, status(false, true), 2000);
    box_lines_on_box_status_changed(&rig.lines, status(true, true), 2100);
    box_lines_on_box_status_changed(&rig.lines, status(true, false), 2200);

    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 0, "STS,EMPTY,LID_OPEN\n");
    CHECK_LINE(&rig, 1, "STS,OCCUPIED,LID_OPEN\n");
    CHECK_LINE(&rig, 2, "STS,OCCUPIED,LID_CLOSED\n");
}

static void test_an_event_writes_one_evt_line_with_sequence_number_1(void)
{
    Rig rig;
    start(&rig);
    box_lines_on_box_status_changed(&rig.lines, status(false, false), 2000);

    box_lines_on_event(&rig.lines, BOX_EVENT_DELIVERED, 9000);

    CHECK_EQ_INT(2, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,1,DELIVERED\n");
}

static void test_each_event_gets_the_next_sequence_number_and_its_outcome_name(void)
{
    Rig rig;
    start(&rig);
    box_lines_on_box_status_changed(&rig.lines, status(false, false), 2000);

    box_lines_on_event(&rig.lines, BOX_EVENT_DELIVERED, 3000);
    box_lines_on_event(&rig.lines, BOX_EVENT_OPENED_OCCUPIED, 4000);
    box_lines_on_event(&rig.lines, BOX_EVENT_COLLECTED, 5000);
    box_lines_on_event(&rig.lines, BOX_EVENT_OPENED_EMPTY, 6000);

    CHECK_EQ_INT(5, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,1,DELIVERED\n");
    CHECK_LINE(&rig, 2, "EVT,2,OPENED_OCCUPIED\n");
    CHECK_LINE(&rig, 3, "EVT,3,COLLECTED\n");
    CHECK_LINE(&rig, 4, "EVT,4,OPENED_EMPTY\n");
}

static void test_multi_digit_sequence_numbers_are_written_in_full(void)
{
    Rig rig;
    int i;
    start(&rig);
    box_lines_on_box_status_changed(&rig.lines, status(false, false), 2000);

    for (i = 0; i < 12; i++) {
        box_lines_on_event(&rig.lines, BOX_EVENT_OPENED_EMPTY, 3000u + (uint32_t)i);
    }

    CHECK_LINE(&rig, 10, "EVT,10,OPENED_EMPTY\n");
    CHECK_LINE(&rig, 12, "EVT,12,OPENED_EMPTY\n");
}

/* Tick every 10 ms from `from_ms` up to and including `to_ms`. */
static void tick_through(Rig *rig, uint32_t from_ms, uint32_t to_ms)
{
    uint32_t now_ms = from_ms;
    for (;;) {
        box_lines_tick(&rig->lines, now_ms);
        if (now_ms == to_ms) {
            break;
        }
        now_ms += 10u;
    }
}

static void test_no_line_at_all_is_written_before_the_first_box_status(void)
{
    Rig rig;
    start(&rig);

    tick_through(&rig, 0, 60000);

    CHECK_EQ_INT(0, rig.recorder.count);
}

static void test_the_last_box_status_is_repeated_as_a_heartbeat(void)
{
    Rig rig;
    start(&rig);
    box_lines_on_box_status_changed(&rig.lines, status(true, false), 2000);

    tick_through(&rig, 2000, 2000 + BOX_STS_HEARTBEAT_MS - 10);
    CHECK_EQ_INT(1, rig.recorder.count);

    tick_through(&rig, 2000 + BOX_STS_HEARTBEAT_MS, 2000 + 2 * BOX_STS_HEARTBEAT_MS);
    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 1, "STS,OCCUPIED,LID_CLOSED\n");
    CHECK_LINE(&rig, 2, "STS,OCCUPIED,LID_CLOSED\n");
}

static void test_a_box_status_change_restarts_the_heartbeat_wait(void)
{
    Rig rig;
    start(&rig);
    box_lines_on_box_status_changed(&rig.lines, status(false, false), 2000);
    tick_through(&rig, 2000, 3000);
    box_lines_on_box_status_changed(&rig.lines, status(false, true), 3000);

    tick_through(&rig, 3000, 3000 + BOX_STS_HEARTBEAT_MS - 10);
    CHECK_EQ_INT(2, rig.recorder.count);

    tick_through(&rig, 3000 + BOX_STS_HEARTBEAT_MS, 3000 + BOX_STS_HEARTBEAT_MS);
    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 2, "STS,EMPTY,LID_OPEN\n");
}

static void test_an_event_does_not_delay_the_heartbeat(void)
{
    Rig rig;
    start(&rig);
    box_lines_on_box_status_changed(&rig.lines, status(true, false), 2000);
    box_lines_on_event(&rig.lines, BOX_EVENT_DELIVERED, 4000);

    tick_through(&rig, 4000, 2000 + BOX_STS_HEARTBEAT_MS);

    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 2, "STS,OCCUPIED,LID_CLOSED\n");
}

static void test_the_heartbeat_keeps_time_across_the_millisecond_wraparound(void)
{
    Rig rig;
    uint32_t changed_at_ms = 0xFFFFFFFFu - 995u; /* a multiple of 10 ms */
    start(&rig);
    box_lines_on_box_status_changed(&rig.lines, status(false, false), changed_at_ms);

    tick_through(&rig, changed_at_ms, changed_at_ms + BOX_STS_HEARTBEAT_MS - 10u);
    CHECK_EQ_INT(1, rig.recorder.count);

    tick_through(&rig, changed_at_ms + BOX_STS_HEARTBEAT_MS,
                 changed_at_ms + BOX_STS_HEARTBEAT_MS);
    CHECK_EQ_INT(2, rig.recorder.count);
}

static BoxReadings readings(bool lid_open, bool parcel_present)
{
    BoxReadings r;
    r.lid_open = lid_open;
    r.parcel_present = parcel_present;
    return r;
}

/* Tick both the core and the lines every 10 ms, as the firmware does. */
static uint32_t run_firmware(Rig *rig, BoxCore *core, uint32_t now_ms,
                             BoxReadings r, uint32_t duration_ms)
{
    uint32_t elapsed_ms;
    /* Count elapsed time, not an end time, so this is safe across wraparound. */
    for (elapsed_ms = 0; elapsed_ms < duration_ms; elapsed_ms += 10u) {
        now_ms += 10u;
        box_core_tick(core, now_ms, r);
        box_lines_tick(&rig->lines, now_ms);
    }
    return now_ms;
}

static void test_wired_to_the_core_a_delivery_writes_its_box_status_and_event_lines(void)
{
    Rig rig;
    BoxCore core;
    uint32_t now_ms = 0;
    start(&rig);
    box_core_init(&core, box_lines_core_outputs(&rig.lines), now_ms, readings(false, false));

    now_ms = run_firmware(&rig, &core, now_ms, readings(false, false), BOX_SETTLE_TIME_MS + 100);
    now_ms = run_firmware(&rig, &core, now_ms, readings(true, false), 1000);
    now_ms = run_firmware(&rig, &core, now_ms, readings(false, true), BOX_SETTLE_TIME_MS + 100);

    CHECK_EQ_INT(5, rig.recorder.count);
    CHECK_LINE(&rig, 0, "STS,EMPTY,LID_CLOSED\n");
    CHECK_LINE(&rig, 1, "STS,EMPTY,LID_OPEN\n");
    CHECK_LINE(&rig, 2, "STS,EMPTY,LID_CLOSED\n");
    CHECK_LINE(&rig, 3, "STS,OCCUPIED,LID_CLOSED\n");
    CHECK_LINE(&rig, 4, "EVT,1,DELIVERED\n");
}

int main(void)
{
    RUN_TEST(test_a_box_status_change_writes_one_sts_line);
    RUN_TEST(test_sts_lines_name_every_box_status);
    RUN_TEST(test_an_event_writes_one_evt_line_with_sequence_number_1);
    RUN_TEST(test_each_event_gets_the_next_sequence_number_and_its_outcome_name);
    RUN_TEST(test_multi_digit_sequence_numbers_are_written_in_full);
    RUN_TEST(test_no_line_at_all_is_written_before_the_first_box_status);
    RUN_TEST(test_the_last_box_status_is_repeated_as_a_heartbeat);
    RUN_TEST(test_a_box_status_change_restarts_the_heartbeat_wait);
    RUN_TEST(test_an_event_does_not_delay_the_heartbeat);
    RUN_TEST(test_the_heartbeat_keeps_time_across_the_millisecond_wraparound);
    RUN_TEST(test_wired_to_the_core_a_delivery_writes_its_box_status_and_event_lines);
    return TEST_SUMMARY();
}
