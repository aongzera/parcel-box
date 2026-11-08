/*
 * PC tests for the ESP32 end of the board link (bridge_link). Each test feeds
 * bytes as the STM32 (or a person at a serial monitor) would send them, then
 * checks only what comes out: the replies, Events and Box statuses.
 *
 * Every line ends in its checksum, `*HH`: the CRC-8 (polynomial 0x1D,
 * initial value 0x00) of every byte before the "*". The checksums here
 * were worked out with an independent Python script, not with the code
 * under test.
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

/* A link as the STM32's: every line must carry a right checksum. */
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

/* A link as the USB serial monitor's, for typed lines. */
static void start_typed(Rig *rig)
{
    start(rig);
    bridge_link_allow_lines_without_checksum(&rig->link);
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
    feed(&rig, "BOOT*B6\n");
    CHECK_EQ_INT(1, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 0, "ACK,0*73\n");
    CHECK_EQ_INT(0, rig.recorder.event_count);
}

static void test_a_repeated_boot_is_answered_again(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOT*B6\nBOOT*B6\n");
    CHECK_EQ_INT(2, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 1, "ACK,0*73\n");
}

/* --- EVT -------------------------------------------------------------- */

static void test_an_event_is_acked_with_its_seq_and_forwarded(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOT*B6\nEVT,1,DELIVERED*4E\n");
    CHECK_EQ_INT(2, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 1, "ACK,1*6E\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK_EQ_INT(1, rig.recorder.events[0].seq);
    CHECK_EQ_INT(BRIDGE_DELIVERED, rig.recorder.events[0].outcome);
}

static void test_each_outcome_name_is_forwarded_as_that_outcome(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "EVT,7,COLLECTED*55\nEVT,8,OPENED_OCCUPIED*63\nEVT,9,OPENED_EMPTY*B2\n");
    CHECK_EQ_INT(3, rig.recorder.event_count);
    CHECK_EQ_INT(BRIDGE_COLLECTED, rig.recorder.events[0].outcome);
    CHECK_EQ_INT(BRIDGE_OPENED_OCCUPIED, rig.recorder.events[1].outcome);
    CHECK_EQ_INT(BRIDGE_OPENED_EMPTY, rig.recorder.events[2].outcome);
    CHECK_REPLY(&rig, 0, "ACK,7*20\n");
    CHECK_REPLY(&rig, 1, "ACK,8*9B\n");
    CHECK_REPLY(&rig, 2, "ACK,9*86\n");
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
    feed(&rig, "EVT,1,DELIVERED*4E\n");
    CHECK_EQ_INT(1, replies_before_event);
    CHECK_REPLY(&rig, 0, "ACK,1*6E\n");
}

static void test_the_largest_seq_is_acked(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "EVT,4294967295,DELIVERED*62\n");
    CHECK_REPLY(&rig, 0, "ACK,4294967295*20\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK(rig.recorder.events[0].seq == 4294967295u);
}

/* --- Checksums -------------------------------------------------------- */

/* On a link that needs checksums, each line must be dropped (reported, but
 * no reply, Event or status), and the link must still work after it. */
static void check_dropped_on(Rig *rig, const char *line)
{
    feed(rig, line);
    if (rig->recorder.reply_count != 0 || rig->recorder.event_count != 0 ||
        rig->recorder.status_count != 0 || rig->recorder.line_count != 0) {
        printf("    not dropped: \"%s\"\n", line);
        test_current_failed = 1;
    }
    CHECK_EQ_INT(1, rig->recorder.dropped_count);
    feed(rig, "EVT,2,DELIVERED*51\n");
    CHECK_EQ_INT(1, rig->recorder.reply_count);
    CHECK_REPLY(rig, 0, "ACK,2*49\n");
    CHECK_EQ_INT(1, rig->recorder.event_count);
}

static void check_dropped(const char *line)
{
    Rig rig;
    start(&rig);
    check_dropped_on(&rig, line);
}

static void check_dropped_when_typed(const char *line)
{
    Rig rig;
    start_typed(&rig);
    check_dropped_on(&rig, line);
}

static void test_a_line_with_no_checksum_is_dropped(void)
{
    check_dropped("BOOT\n");
    check_dropped("EVT,1,DELIVERED\n");
    check_dropped("STS,EMPTY,LID_OPEN\n");
}

static void test_a_line_with_a_wrong_or_malformed_checksum_is_dropped(void)
{
    check_dropped("BOOT*B7\n");                 /* wrong */
    check_dropped("BOOT*6B\n");                 /* the right digits, swapped */
    check_dropped("BOOT*\n");
    check_dropped("BOOT*B\n");                  /* one digit */
    check_dropped("BOOT*0B6\n");                /* three digits */
    check_dropped("BOOT*BG\n");                 /* not hex */
    check_dropped("BOOT*B6 \n");                /* something after it */
    check_dropped("BOOT *B6\n");                /* "BOOT"'s, not "BOOT "'s */
    check_dropped("BOOT*B6*B6\n");
    check_dropped("EVT,1,DELIVERED*4F\n");
}

static void test_a_lowercase_checksum_is_read_like_an_uppercase_one(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "EVT,3,COLLECTED*8a\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK_REPLY(&rig, 0, "ACK,3*54\n");
}

static void test_a_flipped_bit_that_makes_evt_12_into_evt_92_is_dropped(void)
{
    /* "EVT,12,DELIVERED*8A" with one bit flipped on the wire ('1' ^ 0x08
     * is '9'): 92 > 12, so if it were accepted, the real Events 13 to 92
     * would be taken for resends. */
    check_dropped("EVT,92,DELIVERED*8A\n");
}

/* `line` is `original` itself, or differs only in the case of its two hex
 * digits (both cases are read): the same line. */
static bool same_line(const char *line, const char *original)
{
    size_t length = strlen(original);
    size_t i;
    if (strlen(line) != length || memcmp(line, original, length - 2u) != 0) {
        return false;
    }
    for (i = length - 2u; i < length; i++) {
        if ((line[i] | 0x20) != (original[i] | 0x20)) {
            return false;
        }
    }
    return true;
}

/* Only bit 0x20 of the "A" in "*8A" may flip and still be read: "*8a" is
 * the same checksum in lowercase, so the same Event. */
static void test_no_single_flipped_bit_in_an_event_line_is_acked_or_forwarded(void)
{
    static const char good[] = "EVT,12,DELIVERED*8A";
    size_t at;
    int bit;

    for (at = 0; at < sizeof good - 1u; at++) {
        for (bit = 0; bit < 8; bit++) {
            char line[sizeof good];
            Rig rig;
            start(&rig);
            memcpy(line, good, sizeof good);
            line[at] = (char)(line[at] ^ (1 << bit));

            feed(&rig, line);
            feed(&rig, "\n");

            if ((rig.recorder.reply_count != 0 || rig.recorder.event_count != 0) &&
                !same_line(line, good)) {
                printf("    bit %d of byte %u flipped: \"%s\" was %s\n", bit,
                       (unsigned)at, line,
                       rig.recorder.event_count != 0 ? "forwarded" : "ACKed");
                test_current_failed = 1;
            }
        }
    }
}

/* The longest line of each kind, sealed with CRC-8 (poly 0x1D, init 0x00)
 * by an independent Python script. */
static const char *const longest_lines[] = {
    "EVT,4294967295,OPENED_OCCUPIED*49",
    "STS,OCCUPIED,LID_CLOSED*39",
    "BOOT*B6",
    "ACK,4294967295*20",
};

static void test_no_two_flipped_bits_in_the_longest_line_of_each_kind_are_read_as_another_line(void)
{
    size_t l;
    int escapes = 0;

    for (l = 0; l < sizeof longest_lines / sizeof longest_lines[0]; l++) {
        const char *good = longest_lines[l];
        size_t bits = strlen(good) * 8u;
        size_t a;
        size_t b;
        for (a = 0; a < bits; a++) {
            for (b = a + 1u; b < bits; b++) {
                char line[40];
                Rig rig;
                int i;
                start(&rig);
                strcpy(line, good);
                line[a / 8u] = (char)(line[a / 8u] ^ (1 << (a % 8u)));
                line[b / 8u] = (char)(line[b / 8u] ^ (1 << (b % 8u)));

                /* Byte by byte: a flip may make a NUL or a "\n". */
                for (i = 0; good[i] != '\0'; i++) {
                    bridge_link_feed(&rig.link, (uint8_t)line[i]);
                }
                bridge_link_feed(&rig.link, '\n');

                for (i = 0; i < rig.recorder.line_count && i < MAX_OUT; i++) {
                    if (!same_line(rig.recorder.lines[i], good) && escapes++ < 5) {
                        printf("    bits %u and %u of \"%s\" flipped: read as \"%s\"\n",
                               (unsigned)a, (unsigned)b, good, rig.recorder.lines[i]);
                    }
                }
            }
        }
    }
    CHECK_EQ_INT(0, escapes);
}

/* The CRC-8 of product.md section 5, written out from the spec to seal the
 * thousands of lines below; the test below checks it against checksums
 * worked out independently before it trusts it. */
static unsigned spec_crc8(const char *body, size_t length)
{
    unsigned crc = 0x00u;
    size_t i;
    int bit;
    for (i = 0; i < length; i++) {
        crc ^= (unsigned char)body[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80u) ? ((crc << 1) ^ 0x1Du) & 0xFFu : (crc << 1) & 0xFFu;
        }
    }
    return crc;
}

static void seal(char *line, const char *body)
{
    sprintf(line, "%s*%02X", body, spec_crc8(body, strlen(body)));
}

typedef struct {
    uint32_t highest_seq;   /* highest seq passed to on_event; 0: none */
} SeqWatch;

static void watch_seq(void *context, BridgeEvent event)
{
    SeqWatch *watch = context;
    if (event.seq > watch->highest_seq) {
        watch->highest_seq = event.seq;
    }
}

/* A fresh link, so it accepts any EVT number: the highest seq it accepts
 * is what a garbled line would push the duplicate check up to. */
static void start_watched(BridgeLink *link, SeqWatch *watch)
{
    BridgeLinkOutputs outputs;
    memset(&outputs, 0, sizeof outputs);
    watch->highest_seq = 0;
    outputs.context = watch;
    outputs.on_event = watch_seq;
    bridge_link_init(link, outputs);
}

/* Byte by byte: a flip may make a NUL or a "\n". */
static void feed_bytes(BridgeLink *link, const char *bytes, size_t length)
{
    size_t i;
    for (i = 0; i < length; i++) {
        bridge_link_feed(link, (uint8_t)bytes[i]);
    }
}

static uint32_t highest_seq_accepted(const char *line, size_t length)
{
    BridgeLink link;
    SeqWatch watch;
    start_watched(&link, &watch);
    feed_bytes(&link, line, length);
    bridge_link_feed(&link, '\n');
    return watch.highest_seq;
}

/* The hex check digits turn one flipped bit into a multi-bit change of the
 * checksum value (issue #32), so a few pairs of one message bit plus one
 * check-digit bit get through. What must never happen is that such a line
 * is accepted as an Event with a higher number than the real one: the
 * duplicate check would then drop the real Events below it. Tried for
 * every Event the Box is likely to number (1 to 1000) with each outcome. */
static void test_no_body_bit_and_check_digit_bit_flipped_together_raise_an_event_number(void)
{
    static const char *const outcomes[] = {
        "DELIVERED", "COLLECTED", "OPENED_OCCUPIED", "OPENED_EMPTY",
    };
    char line[BRIDGE_LINK_LINE_MAX + 1];
    unsigned seq;
    size_t o;
    int raised = 0;

    /* The sealer agrees with checksums worked out independently. */
    seal(line, "EVT,1,DELIVERED");
    CHECK(strcmp(line, "EVT,1,DELIVERED*4E") == 0);
    seal(line, "EVT,4294967295,OPENED_OCCUPIED");
    CHECK(strcmp(line, "EVT,4294967295,OPENED_OCCUPIED*49") == 0);

    for (seq = 1; seq <= 1000; seq++) {
        for (o = 0; o < sizeof outcomes / sizeof outcomes[0]; o++) {
            char body[BRIDGE_LINK_LINE_MAX + 1];
            size_t body_length;
            size_t length;
            size_t a;
            size_t b;
            sprintf(body, "EVT,%u,%s", seq, outcomes[o]);
            seal(line, body);
            body_length = strlen(body);
            length = strlen(line);
            /* The unflipped line is the Event itself. */
            CHECK(highest_seq_accepted(line, length) == seq);

            for (a = 0; a < body_length * 8u; a++) {
                BridgeLink after_body;
                SeqWatch watch;
                SeqWatch watch_after_body;
                line[a / 8u] = (char)(line[a / 8u] ^ (1 << (a % 8u)));
                /* Feed the flipped body and its "*" once, then each check
                 * digit flip from a copy of the link at that point (a
                 * BridgeLink is a plain value), to keep this under a
                 * second. The copy reports to `watch`, so it is reset
                 * to its state after the body each time. */
                start_watched(&after_body, &watch);
                feed_bytes(&after_body, line, body_length + 1u);
                watch_after_body = watch;
                for (b = (body_length + 1u) * 8u; b < length * 8u; b++) {
                    BridgeLink link = after_body;
                    watch = watch_after_body;
                    line[b / 8u] = (char)(line[b / 8u] ^ (1 << (b % 8u)));
                    feed_bytes(&link, line + body_length + 1u, 2u);
                    bridge_link_feed(&link, '\n');
                    if (watch.highest_seq > seq && raised++ < 5) {
                        printf("    bits %u and %u of EVT,%u,%s flipped: \"%s\" accepted as Event %lu\n",
                               (unsigned)a, (unsigned)b, seq, outcomes[o], line,
                               (unsigned long)watch.highest_seq);
                    }
                    line[b / 8u] = (char)(line[b / 8u] ^ (1 << (b % 8u)));
                }
                line[a / 8u] = (char)(line[a / 8u] ^ (1 << (a % 8u)));
            }
        }
    }
    CHECK_EQ_INT(0, raised);
}

static void test_the_line_passed_on_for_the_debug_log_keeps_its_checksum(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOT*B6\r\n");
    CHECK_EQ_INT(1, rig.recorder.line_count);
    CHECK(strcmp(rig.recorder.lines[0], "BOOT*B6") == 0);
}

/* --- Typed lines (the USB link) ---------------------------------------- */

static void test_a_typed_line_needs_no_checksum(void)
{
    Rig rig;
    start_typed(&rig);
    feed(&rig, "BOOT\nEVT,1,DELIVERED\r\nSTS,EMPTY,LID_OPEN\n");
    CHECK_EQ_INT(2, rig.recorder.reply_count);
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK_EQ_INT(1, rig.recorder.events[0].seq);
    CHECK_EQ_INT(1, rig.recorder.status_count);
    CHECK_EQ_INT(0, rig.recorder.dropped_count);
}

static void test_a_typed_line_with_a_right_checksum_is_read_too(void)
{
    Rig rig;
    start_typed(&rig);
    feed(&rig, "BOOT*B6\nEVT,1,DELIVERED*4E\n");
    CHECK_EQ_INT(2, rig.recorder.reply_count);
    CHECK_EQ_INT(1, rig.recorder.event_count);
}

static void test_the_replies_to_typed_lines_carry_a_checksum(void)
{
    Rig rig;
    start_typed(&rig);
    feed(&rig, "BOOT\nEVT,1,DELIVERED\n");
    CHECK_REPLY(&rig, 0, "ACK,0*73\n");
    CHECK_REPLY(&rig, 1, "ACK,1*6E\n");
}

static void test_a_typed_line_with_a_wrong_or_malformed_checksum_is_dropped(void)
{
    check_dropped_when_typed("BOOT*B7\n");
    check_dropped_when_typed("EVT,1,DELIVERED*4F\n");
    check_dropped_when_typed("EVT,92,DELIVERED*8A\n");
    check_dropped_when_typed("BOOT*\n");
    check_dropped_when_typed("BOOT*B\n");
    check_dropped_when_typed("BOOT*BG\n");
    check_dropped_when_typed("BOOT*B6 \n");
    check_dropped_when_typed("BOO*T\n");        /* any "*" means a checksum */
}

/* --- Duplicates ------------------------------------------------------- */

static void test_a_resent_event_is_acked_again_but_not_forwarded(void)
{
    Rig rig;
    start(&rig);
    /* The STM32 didn't get the first ACK, so it sends the Event again. */
    feed(&rig, "BOOT*B6\nEVT,1,DELIVERED*4E\nEVT,1,DELIVERED*4E\n");
    CHECK_EQ_INT(3, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 1, "ACK,1*6E\n");
    CHECK_REPLY(&rig, 2, "ACK,1*6E\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
}

static void test_an_event_older_than_the_last_accepted_is_acked_but_not_forwarded(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOT*B6\nEVT,1,DELIVERED*4E\nEVT,2,COLLECTED*74\nEVT,1,DELIVERED*4E\n");
    CHECK_EQ_INT(4, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 3, "ACK,1*6E\n");
    CHECK_EQ_INT(2, rig.recorder.event_count);
    CHECK_EQ_INT(2, rig.recorder.events[1].seq);
}

static void test_a_gap_in_the_numbers_is_accepted(void)
{
    Rig rig;
    start(&rig);
    /* The STM32's outbox was full, so Events 2 to 4 were dropped there. */
    feed(&rig, "BOOT*B6\nEVT,1,DELIVERED*4E\nEVT,5,COLLECTED*B4\n");
    CHECK_REPLY(&rig, 2, "ACK,5*1A\n");
    CHECK_EQ_INT(2, rig.recorder.event_count);
    CHECK_EQ_INT(5, rig.recorder.events[1].seq);
}

static void test_after_start_the_first_event_is_accepted_whatever_its_number(void)
{
    Rig rig;
    start(&rig);
    /* The ESP32 restarted mid-session: no BOOT comes, as the STM32 didn't. */
    feed(&rig, "EVT,5,DELIVERED*91\nEVT,3,COLLECTED*8A\n");
    CHECK_EQ_INT(2, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 0, "ACK,5*1A\n");
    CHECK_REPLY(&rig, 1, "ACK,3*54\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK_EQ_INT(5, rig.recorder.events[0].seq);
}

static void test_boot_starts_a_new_session_so_event_1_is_accepted_again(void)
{
    Rig rig;
    start(&rig);
    /* The STM32 rebooted after Event 3; its numbers start again at 1. */
    feed(&rig, "BOOT*B6\nEVT,1,DELIVERED*4E\nEVT,3,COLLECTED*8A\n");
    feed(&rig, "BOOT*B6\nEVT,1,OPENED_EMPTY*CE\n");
    CHECK_REPLY(&rig, 3, "ACK,0*73\n");
    CHECK_REPLY(&rig, 4, "ACK,1*6E\n");
    CHECK_EQ_INT(3, rig.recorder.event_count);
    CHECK_EQ_INT(1, rig.recorder.events[2].seq);
    CHECK_EQ_INT(BRIDGE_OPENED_EMPTY, rig.recorder.events[2].outcome);
}

static void test_a_repeated_boot_is_acked_each_time_and_the_event_after_is_forwarded_once(void)
{
    Rig rig;
    start(&rig);
    /* The first ACK,0 was lost, so the STM32 sent BOOT again. */
    feed(&rig, "BOOT*B6\nBOOT*B6\nEVT,1,DELIVERED*4E\n");
    CHECK_EQ_INT(3, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 0, "ACK,0*73\n");
    CHECK_REPLY(&rig, 1, "ACK,0*73\n");
    CHECK_REPLY(&rig, 2, "ACK,1*6E\n");
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
    feed(&rig, "EVT,2,DELIVERED*51\n");
    CHECK_EQ_INT(1, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 0, "ACK,2*49\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
}

/* The malformed lines below each carry the right checksum for their body,
 * so it is the body that is rejected. */
static void test_an_event_with_a_bad_seq_is_not_acked_or_forwarded(void)
{
    check_ignored("EVT,,DELIVERED*AB\n");
    check_ignored("EVT,0,DELIVERED*B0\n");             /* ACK,0 belongs to BOOT */
    check_ignored("EVT,01,DELIVERED*4C\n");
    check_ignored("EVT,-1,DELIVERED*41\n");
    check_ignored("EVT,1x,DELIVERED*B9\n");
    check_ignored("EVT,4294967296,DELIVERED*7D\n");    /* more than 32 bits */
    check_ignored("EVT,99999999999,DELIVERED*62\n");
    check_ignored("EVT,DELIVERED*84\n");
}

static void test_an_event_with_a_bad_outcome_is_not_acked_or_forwarded(void)
{
    check_ignored("EVT,1,*50\n");
    check_ignored("EVT,1*B3\n");
    check_ignored("EVT,1,DELIVER*19\n");
    check_ignored("EVT,1,delivered*53\n");
    check_ignored("EVT,1,DELIVERED,*AE\n");
    check_ignored("EVT,1,DELIVERED *32\n");
}

/* --- STS -------------------------------------------------------------- */

static void test_a_box_status_is_forwarded_without_a_reply(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "STS,OCCUPIED,LID_CLOSED*39\nSTS,EMPTY,LID_OPEN*01\n");
    CHECK_EQ_INT(0, rig.recorder.reply_count);
    CHECK_EQ_INT(2, rig.recorder.status_count);
    CHECK(rig.recorder.statuses[0].occupied);
    CHECK(!rig.recorder.statuses[0].lid_open);
    CHECK(!rig.recorder.statuses[1].occupied);
    CHECK(rig.recorder.statuses[1].lid_open);
}

static void test_a_garbled_box_status_is_ignored(void)
{
    check_ignored("STS*7E\n");
    check_ignored("STS,OCCUPIED*03\n");
    check_ignored("STS,OCCUPIED,*3C\n");
    check_ignored("STS,FULL,LID_CLOSED*95\n");
    check_ignored("STS,OCCUPIED,LID_SHUT*13\n");
    check_ignored("STS,LID_CLOSED,OCCUPIED*0F\n");
    check_ignored("STS,OCCUPIED,LID_CLOSED,*A4\n");
}

/* --- Reading lines ---------------------------------------------------- */

static void test_crlf_line_endings_are_read_like_lf(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOT*B6\r\nEVT,1,COLLECTED*6B\r\nSTS,EMPTY,LID_CLOSED*A5\r\n");
    CHECK_EQ_INT(2, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 0, "ACK,0*73\n");
    CHECK_REPLY(&rig, 1, "ACK,1*6E\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK_EQ_INT(1, rig.recorder.status_count);
}

static void test_unknown_and_empty_lines_are_ignored(void)
{
    check_ignored("HELLO*F0\n");
    check_ignored("ACK,1*6E\n");
    check_ignored("boot*3E\n");
    check_ignored("BOOT,1*E8\n");
    check_ignored(" BOOT*77\n");
    check_ignored("\n");
    check_ignored("\r\n");
    check_ignored("EVT*A8\n");
    check_ignored("*00\n");   /* an empty body */
}

/* `count` copies of `c`, NUL-terminated, in `out`. */
static const char *repeat(char *out, char c, int count)
{
    memset(out, c, (size_t)count);
    out[count] = '\0';
    return out;
}

/* The checksum of n 'x's, for n = 0 to 39, worked out with Python. */
static const char *const x_checksums[40] = {
    "00", "B1", "C0", "84", "E3", "24", "42", "98", "B2", "E7",
    "50", "6F", "9E", "FC", "52", "55", "06", "FF", "75", "81",
    "8A", "45", "CB", "4B", "6D", "A4", "64", "51", "72", "D2",
    "73", "CF", "3F", "40", "A2", "2A", "E4", "77", "BB", "12",
};

/* A line of `length` characters (4 to 42) with a right checksum:
 * "xxx...x*HH". */
static const char *x_line(char *out, int length)
{
    int xs = length - 3;
    repeat(out, 'x', xs);
    strcat(out, "*");
    strcat(out, x_checksums[xs]);
    return out;
}

static void test_an_overlong_line_is_dropped_whole(void)
{
    char line[BRIDGE_LINK_LINE_MAX + 8];
    Rig rig;

    /* One character too many, with a right checksum. */
    start(&rig);
    feed(&rig, x_line(line, BRIDGE_LINK_LINE_MAX + 1));
    feed(&rig, "\n");
    CHECK_EQ_INT(1, rig.recorder.dropped_count);
    CHECK_EQ_INT(0, rig.recorder.line_count);

    /* Then a valid line's worth at the end, which must not leak out. */
    check_ignored("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxBOOT*B6\n");
    check_ignored("EVT,1,DELIVEREDxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx*29\n");
}

static void test_a_line_of_the_longest_length_is_kept(void)
{
    char line[BRIDGE_LINK_LINE_MAX + 4];
    Rig rig;
    start(&rig);
    feed(&rig, x_line(line, BRIDGE_LINK_LINE_MAX));
    feed(&rig, "\r\n");
    CHECK_EQ_INT(1, rig.recorder.line_count);
    CHECK_EQ_INT(0, rig.recorder.dropped_count);
}

static void test_the_longest_line_the_stm32_sends_is_read(void)
{
    Rig rig;
    start(&rig);
    /* 33 characters, with a "\r\n" ending. */
    feed(&rig, "EVT,4294967295,OPENED_OCCUPIED*49\r\n");
    CHECK_REPLY(&rig, 0, "ACK,4294967295*20\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
}

static void test_a_line_with_a_non_printable_byte_is_dropped(void)
{
    /* Even with a checksum that counts the bad byte. */
    check_ignored("BO\x01OT*DB\n");
    check_ignored("EVT,1,DELIVERED\x80*93\n");
    check_ignored("BOOT\r*A3\r\n");
    check_ignored("BOOT\t*D7\n");
}

static void test_a_line_that_lost_bytes_is_dropped(void)
{
    Rig rig;
    start(&rig);
    /* Bytes lost in "EVT,1,DELIVERED*4E": even though what arrived checks
     * out, the line is dropped. */
    feed(&rig, "BOOT*B6\nEVT,1");
    bridge_link_lost(&rig.link);
    feed(&rig, ",DELIVERED*4E\n");
    CHECK_EQ_INT(1, rig.recorder.dropped_count);
    CHECK_EQ_INT(1, rig.recorder.reply_count);
    CHECK_EQ_INT(0, rig.recorder.event_count);
}

static void test_after_lost_bytes_every_line_is_dropped_until_the_line_goes_quiet(void)
{
    Rig rig;
    start(&rig);
    /* The loss is reported late: the bytes before the gap (a whole STS
     * line) were still buffered, and the gap fell inside the EVT line. */
    feed(&rig, "BOOT*B6\n");
    bridge_link_lost(&rig.link);
    feed(&rig, "STS,EMPTY,LID_CLOSED*A5\nEVT,1");
    feed(&rig, ",DELIVERED*4E\n");
    CHECK_EQ_INT(2, rig.recorder.dropped_count);
    CHECK_EQ_INT(0, rig.recorder.status_count);
    CHECK_EQ_INT(1, rig.recorder.reply_count);   /* BOOT's ACK,0 only */
    CHECK_EQ_INT(0, rig.recorder.event_count);

    bridge_link_quiet(&rig.link);
    feed(&rig, "EVT,2,DELIVERED*51\n");
    CHECK_EQ_INT(2, rig.recorder.reply_count);
    CHECK_REPLY(&rig, 1, "ACK,2*49\n");
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK_EQ_INT(2, rig.recorder.events[0].seq);
}

static void test_a_line_begun_before_the_line_went_quiet_is_dropped(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOT*B6\n");
    bridge_link_lost(&rig.link);
    feed(&rig, "EVT,1");   /* its "\n" was lost, or comes after the quiet */
    bridge_link_quiet(&rig.link);
    feed(&rig, ",DELIVERED*4E\nEVT,3,COLLECTED*8A\n");
    CHECK_EQ_INT(1, rig.recorder.dropped_count);
    CHECK_EQ_INT(1, rig.recorder.event_count);
    CHECK_EQ_INT(3, rig.recorder.events[0].seq);
}

static void test_quiet_with_no_bytes_lost_changes_nothing(void)
{
    Rig rig;
    start(&rig);
    /* main.cpp may report every quiet spell, so mid-line is harmless. */
    feed(&rig, "BOOT*B6\nEVT,1");
    bridge_link_quiet(&rig.link);
    feed(&rig, ",DELIVERED*4E\n");
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
    feed(&rig, "BOOT*B6\r\nHELLO*F0\nEVT,1,DELIVERED*4E\n");
    CHECK_EQ_INT(3, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "BOOT*B6");
    CHECK_LINE(&rig, 1, "HELLO*F0");
    CHECK_LINE(&rig, 2, "EVT,1,DELIVERED*4E");
    CHECK_EQ_INT(0, rig.recorder.dropped_count);
}

static void test_a_dropped_line_is_reported_and_not_passed_on(void)
{
    Rig rig;
    start(&rig);
    feed(&rig, "BOOX\bT*47\n");   /* a typo fixed with backspace */
    feed(&rig, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n");
    feed(&rig, "BOOT*B7\n");      /* a wrong checksum */
    CHECK_EQ_INT(3, rig.recorder.dropped_count);
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
    const char *bytes = "BOOT*B6\nEVT,1,DELIVERED*4E\nSTS,EMPTY,LID_OPEN*01\nHELLO*F0\n"
                        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\nBOOT*B7\n"
                        "EVT,2,COLLECTED*74\n";
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
    RUN_TEST(test_a_line_with_no_checksum_is_dropped);
    RUN_TEST(test_a_line_with_a_wrong_or_malformed_checksum_is_dropped);
    RUN_TEST(test_a_lowercase_checksum_is_read_like_an_uppercase_one);
    RUN_TEST(test_a_flipped_bit_that_makes_evt_12_into_evt_92_is_dropped);
    RUN_TEST(test_no_single_flipped_bit_in_an_event_line_is_acked_or_forwarded);
    RUN_TEST(test_no_two_flipped_bits_in_the_longest_line_of_each_kind_are_read_as_another_line);
    RUN_TEST(test_no_body_bit_and_check_digit_bit_flipped_together_raise_an_event_number);
    RUN_TEST(test_the_line_passed_on_for_the_debug_log_keeps_its_checksum);
    RUN_TEST(test_a_typed_line_needs_no_checksum);
    RUN_TEST(test_a_typed_line_with_a_right_checksum_is_read_too);
    RUN_TEST(test_the_replies_to_typed_lines_carry_a_checksum);
    RUN_TEST(test_a_typed_line_with_a_wrong_or_malformed_checksum_is_dropped);
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
    RUN_TEST(test_a_line_of_the_longest_length_is_kept);
    RUN_TEST(test_the_longest_line_the_stm32_sends_is_read);
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
