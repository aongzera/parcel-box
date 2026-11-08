/*
 * PC tests for the ESP32 end of the board link (bridge_link). Each test feeds
 * bytes as the STM32 (or a person at a serial monitor) would send them, then
 * checks only what comes out: the replies, Events and Box statuses.
 */
#include <string.h>

#include "bridge_link.h"
#include "test_harness.h"

#define MAX_OUT 16
#define OUT_LEN 64

typedef struct {
    char replies[MAX_OUT][OUT_LEN];
    int reply_count;
    BridgeEvent events[MAX_OUT];
    int event_count;
    BridgeStatus statuses[MAX_OUT];
    int status_count;
    char lines[MAX_OUT][OUT_LEN];
    int line_count;
    int dropped_count;
} Recorder;

static void record_line(void *context, const char *line)
{
    Recorder *r = context;
    if (r->line_count < MAX_OUT) {
        strncpy(r->lines[r->line_count], line, OUT_LEN - 1);
        r->lines[r->line_count][OUT_LEN - 1] = '\0';
    }
    r->line_count++;
}

static void record_dropped(void *context)
{
    Recorder *r = context;
    r->dropped_count++;
}

static void record_reply(void *context, const char *line)
{
    Recorder *r = context;
    if (r->reply_count < MAX_OUT) {
        strncpy(r->replies[r->reply_count], line, OUT_LEN - 1);
        r->replies[r->reply_count][OUT_LEN - 1] = '\0';
    }
    r->reply_count++;
}

static void record_event(void *context, BridgeEvent event)
{
    Recorder *r = context;
    if (r->event_count < MAX_OUT) {
        r->events[r->event_count] = event;
    }
    r->event_count++;
}

static void record_status(void *context, BridgeStatus status)
{
    Recorder *r = context;
    if (r->status_count < MAX_OUT) {
        r->statuses[r->status_count] = status;
    }
    r->status_count++;
}

typedef struct {
    BridgeLink link;
    Recorder recorder;
} Rig;

static void start(Rig *rig)
{
    BridgeLinkOutputs outputs;
    memset(rig, 0, sizeof *rig);
    outputs.context = &rig->recorder;
    outputs.write_line = record_reply;
    outputs.on_event = record_event;
    outputs.on_status = record_status;
    outputs.on_line = record_line;
    outputs.on_dropped = record_dropped;
    bridge_link_init(&rig->link, outputs);
}

static void feed(Rig *rig, const char *bytes)
{
    while (*bytes != '\0') {
        bridge_link_feed(&rig->link, (uint8_t)*bytes++);
    }
}

#define CHECK_REPLY(rig, index, expected)                                   \
    do {                                                                    \
        CHECK((index) < MAX_OUT && (rig)->recorder.reply_count > (index));  \
        if ((index) < MAX_OUT && (rig)->recorder.reply_count > (index) &&   \
            strcmp((rig)->recorder.replies[(index)], (expected)) != 0) {    \
            printf("    %s:%d: reply %d: expected \"%s\", got \"%s\"\n",    \
                   __FILE__, __LINE__, (index), (expected),                 \
                   (rig)->recorder.replies[(index)]);                       \
            test_current_failed = 1;                                        \
        }                                                                   \
    } while (0)

/* --- BOOT ------------------------------------------------------------- */

static void test_boot_is_answered_with_ack_0(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOT\n");
    CHECK_EQ_INT(1, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 0, "ACK,0\n");
    CHECK_EQ_INT(0, rig.recorder.event_count);
}

static void test_a_repeated_boot_is_answered_again(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOT\nBOOT\n");
    CHECK_EQ_INT(2, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 1, "ACK,0\n");
}

/* --- EVT -------------------------------------------------------------- */

static void test_an_event_is_acked_with_its_seq_and_forwarded(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOT\nEVT,1,DELIVERED\n");
    CHECK_EQ_INT(2, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 1, "ACK,1\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK_EQ_INT(1, rig.recorder.events[0].seq);
    CHECK_EQ_INT(BRIDGE_DELIVERED, rig.recorder.events[0].outcome);
}

static void test_each_outcome_name_is_forwarded_as_that_outcome(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "EVT,7,COLLECTED\nEVT,8,OPENED_OCCUPIED\nEVT,9,OPENED_EMPTY\n");
    CHECK_EQ_INT(3, rig.recorder.event_count);
    CHECK_EQ_INT(BRIDGE_COLLECTED, rig.recorder.events[0].outcome);
    CHECK_EQ_INT(BRIDGE_OPENED_OCCUPIED, rig.recorder.events[1].outcome);
    CHECK_EQ_INT(BRIDGE_OPENED_EMPTY, rig.recorder.events[2].outcome);
    CHECK_REPLY(&rig, 0, "ACK,7\n");
    CHECK_REPLY(&rig, 1, "ACK,8\n");
    CHECK_REPLY(&rig, 2, "ACK,9\n");
}

/* How many replies had gone out when on_event was called. */
static int replies_before_event = -1;

static void note_replies_before_event(void *context, BridgeEvent event)
{
    (void)event;
    replies_before_event = ((Recorder *)context)->reply_count;
}

static void test_an_event_is_acked_before_it_is_handed_on(void)
{
    /* So the ACK never waits for on_event (the offline queue, an upload),
     * and the STM32's outbox keeps moving while the ESP32 is offline. */
    Rig rig;
    BridgeLinkOutputs outputs;
    memset(&rig, 0, sizeof rig);
    memset(&outputs, 0, sizeof outputs);
    outputs.context = &rig.recorder;
    outputs.write_line = record_reply;
    outputs.on_event = note_replies_before_event;
    bridge_link_init(&rig.link, outputs);
    replies_before_event = -1;
    feed(&rig, "EVT,1,DELIVERED\n");
    CHECK_EQ_INT(1, replies_before_event);
    CHECK_REPLY(&rig, 0, "ACK,1\n");
}

static void test_the_largest_seq_is_acked(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "EVT,4294967295,DELIVERED\n");
    CHECK_REPLY(&rig, 0, "ACK,4294967295\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK(rig.recorder.events[0].seq == 4294967295u);
}

/* --- Duplicates ------------------------------------------------------- */

static void test_a_resent_event_is_acked_again_but_not_forwarded(void)
{
    Rig rig;
    start(&rig);
    /* The STM32 didn't get the first ACK, so it sends the Event again. */
    feed(&rig, "BOOT\nEVT,1,DELIVERED\nEVT,1,DELIVERED\n");
    CHECK_EQ_INT(3, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 1, "ACK,1\n");
    CHECK_REPLY(&rig, 2, "ACK,1\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
}

static void test_an_event_older_than_the_last_accepted_is_acked_but_not_forwarded(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOT\nEVT,1,DELIVERED\nEVT,2,COLLECTED\nEVT,1,DELIVERED\n");
    CHECK_EQ_INT(4, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 3, "ACK,1\n");
    CHECK_EQ_INT(2, rig.recorder.event_count);
    CHECK_EQ_INT(2, rig.recorder.events[1].seq);
}

static void test_a_gap_in_the_numbers_is_accepted(void)
{
    Rig rig;
    start(&rig);
    /* The STM32's outbox was full, so Events 2 to 4 were dropped there. */
    feed(&rig, "BOOT\nEVT,1,DELIVERED\nEVT,5,COLLECTED\n");
    CHECK_REPLY(&rig, 2, "ACK,5\n");
    CHECK_EQ_INT(2, rig.recorder.event_count);
    CHECK_EQ_INT(5, rig.recorder.events[1].seq);
}

static void test_after_start_the_first_event_is_accepted_whatever_its_number(void)
{
    Rig rig;
    start(&rig);
    /* The ESP32 restarted mid-session: no BOOT comes, as the STM32 didn't. */
    feed(&rig, "EVT,5,DELIVERED\nEVT,3,COLLECTED\n");
    CHECK_EQ_INT(2, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 0, "ACK,5\n");
    CHECK_REPLY(&rig, 1, "ACK,3\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK_EQ_INT(5, rig.recorder.events[0].seq);
}

static void test_boot_starts_a_new_session_so_event_1_is_accepted_again(void)
{
    Rig rig;
    start(&rig);
    /* The STM32 rebooted after Event 3; its numbers start again at 1. */
    feed(&rig, "BOOT\nEVT,1,DELIVERED\nEVT,3,COLLECTED\n");
    feed(&rig, "BOOT\nEVT,1,OPENED_EMPTY\n");
    CHECK_REPLY(&rig, 3, "ACK,0\n");
    CHECK_REPLY(&rig, 4, "ACK,1\n");
    CHECK_EQ_INT(3, rig.recorder.event_count);
    CHECK_EQ_INT(1, rig.recorder.events[2].seq);
    CHECK_EQ_INT(BRIDGE_OPENED_EMPTY, rig.recorder.events[2].outcome);
}

static void test_a_repeated_boot_is_acked_each_time_and_the_event_after_is_forwarded_once(void)
{
    Rig rig;
    start(&rig);
    /* The first ACK,0 was lost, so the STM32 sent BOOT again. */
    feed(&rig, "BOOT\nBOOT\nEVT,1,DELIVERED\n");
    CHECK_EQ_INT(3, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 0, "ACK,0\n");
    CHECK_REPLY(&rig, 1, "ACK,0\n");
    CHECK_REPLY(&rig, 2, "ACK,1\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK_EQ_INT(1, rig.recorder.events[0].seq);
}

/* Each line must get no reply and forward nothing, and the link must still
 * work after it. */
static void check_ignored(const char *line)
{
    Rig rig;
    start(&rig);
    feed(&rig, line);
    if (rig.recorder.reply_count != 0 || rig.recorder.event_count != 0 ||
        rig.recorder.status_count != 0) {
        printf("    not ignored: \"%s\"\n", line);
    }
    CHECK_EQ_INT(0, rig.recorder.reply_count);
    CHECK_EQ_INT(0, rig.recorder.event_count);
    CHECK_EQ_INT(0, rig.recorder.status_count);
    feed(&rig, "EVT,2,DELIVERED\n");
    CHECK_EQ_INT(1, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 0, "ACK,2\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
}

static void test_an_event_with_a_bad_seq_is_not_acked_or_forwarded(void)
{
    check_ignored("EVT,,DELIVERED\n");
    check_ignored("EVT,0,DELIVERED\n");             /* ACK,0 belongs to BOOT */
    check_ignored("EVT,01,DELIVERED\n");
    check_ignored("EVT,-1,DELIVERED\n");
    check_ignored("EVT,1x,DELIVERED\n");
    check_ignored("EVT,4294967296,DELIVERED\n");    /* more than 32 bits */
    check_ignored("EVT,99999999999,DELIVERED\n");
    check_ignored("EVT,DELIVERED\n");
}

static void test_an_event_with_a_bad_outcome_is_not_acked_or_forwarded(void)
{
    check_ignored("EVT,1,\n");
    check_ignored("EVT,1\n");
    check_ignored("EVT,1,DELIVER\n");
    check_ignored("EVT,1,delivered\n");
    check_ignored("EVT,1,DELIVERED,\n");
    check_ignored("EVT,1,DELIVERED \n");
}

/* --- STS -------------------------------------------------------------- */

static void test_a_box_status_is_forwarded_without_a_reply(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "STS,OCCUPIED,LID_CLOSED\nSTS,EMPTY,LID_OPEN\n");
    CHECK_EQ_INT(0, rig.recorder.reply_count);
    CHECK_EQ_INT(2, rig.recorder.status_count);
    CHECK(rig.recorder.statuses[0].occupied);
    CHECK(!rig.recorder.statuses[0].lid_open);
    CHECK(!rig.recorder.statuses[1].occupied);
    CHECK(rig.recorder.statuses[1].lid_open);
}

static void test_a_garbled_box_status_is_ignored(void)
{
    check_ignored("STS\n");
    check_ignored("STS,OCCUPIED\n");
    check_ignored("STS,OCCUPIED,\n");
    check_ignored("STS,FULL,LID_CLOSED\n");
    check_ignored("STS,OCCUPIED,LID_SHUT\n");
    check_ignored("STS,LID_CLOSED,OCCUPIED\n");
    check_ignored("STS,OCCUPIED,LID_CLOSED,\n");
}

/* --- Reading lines ---------------------------------------------------- */

static void test_crlf_line_endings_are_read_like_lf(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOT\r\nEVT,1,COLLECTED\r\nSTS,EMPTY,LID_CLOSED\r\n");
    CHECK_EQ_INT(2, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 0, "ACK,0\n");
    CHECK_REPLY(&rig, 1, "ACK,1\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK_EQ_INT(1, rig.recorder.status_count);
}

static void test_unknown_and_empty_lines_are_ignored(void)
{
    check_ignored("HELLO\n");
    check_ignored("ACK,1\n");
    check_ignored("boot\n");
    check_ignored("BOOT,1\n");
    check_ignored(" BOOT\n");
    check_ignored("\n");
    check_ignored("\r\n");
    check_ignored("EVT\n");
}

static void test_an_overlong_line_is_dropped_whole(void)
{
    /* 33 characters, then a valid line's worth that must not leak out. */
    check_ignored("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n");
    check_ignored("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxBOOT\n");
    check_ignored("EVT,1,DELIVEREDxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n");
}

static void test_a_line_of_the_longest_length_is_read(void)
{
    Rig rig;
    start(&rig);
    /* The longest line the STM32 sends, with a "\r\n" ending. */
    feed(&rig, "EVT,4294967295,OPENED_OCCUPIED\r\n");
    CHECK_REPLY(&rig, 0, "ACK,4294967295\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
}

static void test_a_line_with_a_non_printable_byte_is_dropped(void)
{
    check_ignored("BO\x01OT\n");
    check_ignored("EVT,1,DELIVERED\x80\n");
    check_ignored("BOOT\r\r\n");
    check_ignored("BOOT\t\n");
}

static void test_a_line_that_lost_bytes_is_dropped(void)
{
    Rig rig;
    start(&rig);
    /* "EVT,12,DELIVERED\n" with the "2" lost would otherwise read as Event 1. */
    feed(&rig, "BOOT\nEVT,1");
    bridge_link_lost(&rig.link);
    feed(&rig, ",DELIVERED\n");
    CHECK_EQ_INT(1, rig.recorder.dropped_count);
    CHECK_EQ_INT(1, rig.recorder.reply_count);
    CHECK_EQ_INT(0, rig.recorder.event_count);
}

static void test_after_lost_bytes_every_line_is_dropped_until_the_line_goes_quiet(void)
{
    Rig rig;
    start(&rig);
    /* The loss is reported late: the bytes before the gap (a whole STS
     * line) were still buffered, and the gap fell inside "EVT,1x". */
    feed(&rig, "BOOT\n");
    bridge_link_lost(&rig.link);
    feed(&rig, "STS,EMPTY,LID_CLOSED\nEVT,1");
    feed(&rig, ",DELIVERED\n");
    CHECK_EQ_INT(2, rig.recorder.dropped_count);
    CHECK_EQ_INT(0, rig.recorder.status_count);
    CHECK_EQ_INT(1, rig.recorder.reply_count);   /* BOOT's ACK,0 only */
    CHECK_EQ_INT(0, rig.recorder.event_count);

    bridge_link_quiet(&rig.link);
    feed(&rig, "EVT,2,DELIVERED\n");
    CHECK_EQ_INT(2, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 1, "ACK,2\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK_EQ_INT(2, rig.recorder.events[0].seq);
}

static void test_a_line_begun_before_the_line_went_quiet_is_dropped(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOT\n");
    bridge_link_lost(&rig.link);
    feed(&rig, "EVT,1");   /* its "\n" was lost, or comes after the quiet */
    bridge_link_quiet(&rig.link);
    feed(&rig, ",DELIVERED\nEVT,3,COLLECTED\n");
    CHECK_EQ_INT(1, rig.recorder.dropped_count);
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK_EQ_INT(3, rig.recorder.events[0].seq);
}

static void test_quiet_with_no_bytes_lost_changes_nothing(void)
{
    Rig rig;
    start(&rig);
    /* main.cpp may report every quiet spell, so mid-line is harmless. */
    feed(&rig, "BOOT\nEVT,1");
    bridge_link_quiet(&rig.link);
    feed(&rig, ",DELIVERED\n");
    CHECK_EQ_INT(0, rig.recorder.dropped_count);
    CHECK_EQ_INT(1, rig.recorder.event_count);
}

static void test_an_empty_line_around_lost_bytes_is_skipped_silently(void)
{
    Rig rig;
    start(&rig);
    bridge_link_lost(&rig.link);
    feed(&rig, "\n\r\n");
    bridge_link_quiet(&rig.link);
    CHECK_EQ_INT(0, rig.recorder.dropped_count);
    CHECK_EQ_INT(0, rig.recorder.line_count);
    CHECK_EQ_INT(0, rig.recorder.reply_count);
}

/* --- Debug log -------------------------------------------------------- */

#define CHECK_LINE(rig, index, expected)                                    \
    do {                                                                    \
        CHECK((index) < MAX_OUT && (rig)->recorder.line_count > (index));   \
        if ((index) < MAX_OUT && (rig)->recorder.line_count > (index) &&    \
            strcmp((rig)->recorder.lines[(index)], (expected)) != 0) {      \
            printf("    %s:%d: line %d: expected \"%s\", got \"%s\"\n",     \
                   __FILE__, __LINE__, (index), (expected),                 \
                   (rig)->recorder.lines[(index)]);                         \
            test_current_failed = 1;                                        \
        }                                                                   \
    } while (0)

static void test_each_kept_line_is_passed_on_for_the_debug_log(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOT\r\nHELLO\nEVT,1,DELIVERED\n");
    CHECK_EQ_INT(3, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "BOOT");
    CHECK_LINE(&rig, 1, "HELLO");
    CHECK_LINE(&rig, 2, "EVT,1,DELIVERED");
    CHECK_EQ_INT(0, rig.recorder.dropped_count);
}

static void test_a_dropped_line_is_reported_and_not_passed_on(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOX\bT\n");   /* a typo fixed with backspace */
    feed(&rig, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n");
    CHECK_EQ_INT(2, rig.recorder.dropped_count);
    CHECK_EQ_INT(0, rig.recorder.line_count);
    CHECK_EQ_INT(0, rig.recorder.reply_count);
}

static void test_an_empty_line_is_neither_kept_nor_dropped(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "\n\r\n");
    CHECK_EQ_INT(0, rig.recorder.line_count);
    CHECK_EQ_INT(0, rig.recorder.dropped_count);
}

static void count_event(void *context, BridgeEvent event)
{
    (void)event;
    (*(int *)context)++;
}

static void test_with_every_other_output_null_events_still_forward(void)
{
    BridgeLink link;
    BridgeLinkOutputs outputs;
    int events = 0;
    const char *bytes = "BOOT\nEVT,1,DELIVERED\nSTS,EMPTY,LID_OPEN\nHELLO\n"
                        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\nEVT,2,COLLECTED\n";
    memset(&outputs, 0, sizeof outputs);
    outputs.context = &events;
    outputs.on_event = count_event;
    bridge_link_init(&link, outputs);
    while (*bytes != '\0') {
        bridge_link_feed(&link, (uint8_t)*bytes++);
    }
    CHECK_EQ_INT(2, events);
}

static void test_an_outcome_name_comes_back_as_its_protocol_name(void)
{
    CHECK(strcmp(bridge_outcome_name(BRIDGE_DELIVERED), "DELIVERED") == 0);
    CHECK(strcmp(bridge_outcome_name(BRIDGE_COLLECTED), "COLLECTED") == 0);
    CHECK(strcmp(bridge_outcome_name(BRIDGE_OPENED_OCCUPIED), "OPENED_OCCUPIED") == 0);
    CHECK(strcmp(bridge_outcome_name(BRIDGE_OPENED_EMPTY), "OPENED_EMPTY") == 0);
}

int main(void)
{
    RUN_TEST(test_boot_is_answered_with_ack_0);
    RUN_TEST(test_a_repeated_boot_is_answered_again);
    RUN_TEST(test_an_event_is_acked_with_its_seq_and_forwarded);
    RUN_TEST(test_each_outcome_name_is_forwarded_as_that_outcome);
    RUN_TEST(test_an_event_is_acked_before_it_is_handed_on);
    RUN_TEST(test_the_largest_seq_is_acked);
    RUN_TEST(test_a_resent_event_is_acked_again_but_not_forwarded);
    RUN_TEST(test_an_event_older_than_the_last_accepted_is_acked_but_not_forwarded);
    RUN_TEST(test_a_gap_in_the_numbers_is_accepted);
    RUN_TEST(test_after_start_the_first_event_is_accepted_whatever_its_number);
    RUN_TEST(test_boot_starts_a_new_session_so_event_1_is_accepted_again);
    RUN_TEST(test_a_repeated_boot_is_acked_each_time_and_the_event_after_is_forwarded_once);
    RUN_TEST(test_an_event_with_a_bad_seq_is_not_acked_or_forwarded);
    RUN_TEST(test_an_event_with_a_bad_outcome_is_not_acked_or_forwarded);
    RUN_TEST(test_a_box_status_is_forwarded_without_a_reply);
    RUN_TEST(test_a_garbled_box_status_is_ignored);
    RUN_TEST(test_crlf_line_endings_are_read_like_lf);
    RUN_TEST(test_unknown_and_empty_lines_are_ignored);
    RUN_TEST(test_an_overlong_line_is_dropped_whole);
    RUN_TEST(test_a_line_of_the_longest_length_is_read);
    RUN_TEST(test_a_line_with_a_non_printable_byte_is_dropped);
    RUN_TEST(test_a_line_that_lost_bytes_is_dropped);
    RUN_TEST(test_after_lost_bytes_every_line_is_dropped_until_the_line_goes_quiet);
    RUN_TEST(test_a_line_begun_before_the_line_went_quiet_is_dropped);
    RUN_TEST(test_quiet_with_no_bytes_lost_changes_nothing);
    RUN_TEST(test_an_empty_line_around_lost_bytes_is_skipped_silently);
    RUN_TEST(test_each_kept_line_is_passed_on_for_the_debug_log);
    RUN_TEST(test_a_dropped_line_is_reported_and_not_passed_on);
    RUN_TEST(test_an_empty_line_is_neither_kept_nor_dropped);
    RUN_TEST(test_with_every_other_output_null_events_still_forward);
    RUN_TEST(test_an_outcome_name_comes_back_as_its_protocol_name);
    return TEST_SUMMARY();
}
