/*
 * PC tests for the UART line reader (box_rx). Each test feeds bytes as the
 * ESP32 would send them, then checks only the lines and ACKs that come out.
 */
#include <string.h>

#include "box_rx.h"
#include "test_harness.h"

#define MAX_OUT 16
#define OUT_LEN 64

typedef struct {
    char lines[MAX_OUT][OUT_LEN];
    int line_count;
    uint32_t acks[MAX_OUT];
    int ack_count;
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

static void record_ack(void *context, uint32_t seq)
{
    Recorder *r = context;
    if (r->ack_count < MAX_OUT) {
        r->acks[r->ack_count] = seq;
    }
    r->ack_count++;
}

typedef struct {
    BoxRx rx;
    Recorder recorder;
} Rig;

static void start(Rig *rig)
{
    BoxRxOutputs outputs;
    memset(rig, 0, sizeof *rig);
    outputs.context = &rig->recorder;
    outputs.on_line = record_line;
    outputs.on_ack = record_ack;
    box_rx_init(&rig->rx, outputs);
}

static void feed(Rig *rig, const char *bytes)
{
    while (*bytes != '\0') {
        box_rx_feed(&rig->rx, (uint8_t)*bytes++);
    }
}

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

#define CHECK_ACK(rig, index, expected)                                     \
    do {                                                                    \
        CHECK((index) < MAX_OUT && (rig)->recorder.ack_count > (index));    \
        if ((index) < MAX_OUT && (rig)->recorder.ack_count > (index)) {     \
            CHECK_EQ_INT((expected), (rig)->recorder.acks[(index)]);        \
        }                                                                   \
    } while (0)

/* ---- Tests --------------------------------------------------------------- */

static void test_an_ack_line_reports_its_sequence_number(void)
{
    Rig rig;
    start(&rig);

    feed(&rig, "ACK,7\n");

    CHECK_EQ_INT(1, rig.recorder.ack_count);
    CHECK_ACK(&rig, 0, 7);
    CHECK_EQ_INT(1, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "ACK,7");
}

static void test_a_crlf_line_ending_is_read_like_a_newline(void)
{
    Rig rig;
    start(&rig);

    feed(&rig, "ACK,12\r\nACK,13\r\n");

    CHECK_EQ_INT(2, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "ACK,12");
    CHECK_LINE(&rig, 1, "ACK,13");
    CHECK_EQ_INT(2, rig.recorder.ack_count);
    CHECK_ACK(&rig, 0, 12);
    CHECK_ACK(&rig, 1, 13);
}

static void test_an_unknown_line_is_passed_on_but_is_not_an_ack(void)
{
    Rig rig;
    start(&rig);

    feed(&rig, "HELLO,1\nACK\n");

    CHECK_EQ_INT(2, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "HELLO,1");
    CHECK_LINE(&rig, 1, "ACK");
    CHECK_EQ_INT(0, rig.recorder.ack_count);
}

static void test_a_malformed_ack_is_passed_on_but_reports_nothing(void)
{
    static const char *const malformed[] = {
        "ACK,",            /* no number */
        "ACK,abc",         /* not a number */
        "ACK,12x",         /* junk after the number */
        "ACK,12,3",        /* an extra field */
        "ACK,-1",          /* a sign */
        "ACK,+1",
        "ACK, 5",          /* a space */
        "ACK,5 ",
        "ack,5",           /* wrong case */
        "ACK;5",
        " ACK,5",
    };
    size_t i;

    for (i = 0; i < sizeof malformed / sizeof malformed[0]; i++) {
        Rig rig;
        start(&rig);

        feed(&rig, malformed[i]);
        feed(&rig, "\n");

        CHECK_EQ_INT(1, rig.recorder.line_count);
        CHECK_LINE(&rig, 0, malformed[i]);
        if (rig.recorder.ack_count != 0) {
            printf("    \"%s\" was read as an ACK\n", malformed[i]);
            test_current_failed = 1;
        }
    }
}

static void test_the_largest_sequence_number_is_read_in_full(void)
{
    Rig rig;
    start(&rig);

    feed(&rig, "ACK,4294967295\n");

    CHECK_EQ_INT(1, rig.recorder.ack_count);
    CHECK_ACK(&rig, 0, 4294967295u);
}

static void test_a_sequence_number_too_big_for_32_bits_is_not_an_ack(void)
{
    static const char *const too_big[] = {
        "ACK,4294967296",      /* UINT32_MAX + 1 */
        "ACK,4294967300",      /* overflows on the last digit */
        "ACK,9999999999",
        "ACK,12345678901",     /* 11 digits */
        "ACK,00000000001",     /* 11 digits, even if the value is small */
    };
    size_t i;

    for (i = 0; i < sizeof too_big / sizeof too_big[0]; i++) {
        Rig rig;
        start(&rig);

        feed(&rig, too_big[i]);
        feed(&rig, "\n");

        CHECK_EQ_INT(1, rig.recorder.line_count);
        if (rig.recorder.ack_count != 0) {
            printf("    \"%s\" was read as ACK %lu\n", too_big[i],
                   (unsigned long)rig.recorder.acks[0]);
            test_current_failed = 1;
        }
    }
}

/* `count` copies of `c`, NUL-terminated, in `out`. */
static const char *repeat(char *out, char c, int count)
{
    memset(out, c, (size_t)count);
    out[count] = '\0';
    return out;
}

static void test_a_line_of_the_longest_length_is_kept(void)
{
    char longest[BOX_RX_LINE_MAX + 1];
    Rig rig;
    start(&rig);

    feed(&rig, repeat(longest, 'x', BOX_RX_LINE_MAX));
    feed(&rig, "\r\n");

    CHECK_EQ_INT(1, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, longest);
}

static void test_an_overlong_line_is_dropped_and_the_next_line_is_read(void)
{
    char filler[200];
    Rig rig;
    start(&rig);

    /* One character too many, then far too many, each ending like an ACK. */
    feed(&rig, repeat(filler, 'x', BOX_RX_LINE_MAX - 4));
    feed(&rig, "ACK,1\n");
    feed(&rig, repeat(filler, '7', 199));
    feed(&rig, "ACK,2\n");
    feed(&rig, "ACK,3\n");

    CHECK_EQ_INT(1, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "ACK,3");
    CHECK_EQ_INT(1, rig.recorder.ack_count);
    CHECK_ACK(&rig, 0, 3);
}

static void test_a_line_with_a_non_printable_byte_is_dropped(void)
{
    Rig rig;
    start(&rig);

    /* Noise as the wire is plugged in, a control byte, a stray "\r", a
     * byte with the top bit set, and a NUL: each spoils its own line only. */
    feed(&rig, "\x01\x7f" "ACK,1\n");
    feed(&rig, "ACK,\r2\n");
    feed(&rig, "ACK,\xb3\n");
    box_rx_feed(&rig.rx, 'A');
    box_rx_feed(&rig.rx, 0x00);
    feed(&rig, "CK,4\n");
    feed(&rig, "ACK,5\n");

    CHECK_EQ_INT(1, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "ACK,5");
    CHECK_EQ_INT(1, rig.recorder.ack_count);
    CHECK_ACK(&rig, 0, 5);
}

static void test_empty_lines_are_ignored(void)
{
    Rig rig;
    start(&rig);

    feed(&rig, "\n\r\n\n");

    CHECK_EQ_INT(0, rig.recorder.line_count);
    CHECK_EQ_INT(0, rig.recorder.ack_count);
}

static void test_a_line_that_lost_bytes_is_dropped_and_the_next_line_is_read(void)
{
    Rig rig;
    start(&rig);

    /* "ACK,123\n" with the "3" lost would otherwise read as ACK 12. */
    feed(&rig, "ACK,12");
    box_rx_lost(&rig.rx);
    feed(&rig, "\nACK,124\n");

    CHECK_EQ_INT(1, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "ACK,124");
    CHECK_EQ_INT(1, rig.recorder.ack_count);
    CHECK_ACK(&rig, 0, 124);
}

static void test_bytes_lost_between_lines_drop_the_line_after(void)
{
    Rig rig;
    start(&rig);

    /* The lost bytes may have held that line's start, or the "\n" before it. */
    feed(&rig, "ACK,1\n");
    box_rx_lost(&rig.rx);
    feed(&rig, "ACK,2\nACK,3\n");

    CHECK_EQ_INT(2, rig.recorder.ack_count);
    CHECK_ACK(&rig, 0, 1);
    CHECK_ACK(&rig, 1, 3);
}

int main(void)
{
    RUN_TEST(test_an_ack_line_reports_its_sequence_number);
    RUN_TEST(test_a_crlf_line_ending_is_read_like_a_newline);
    RUN_TEST(test_an_unknown_line_is_passed_on_but_is_not_an_ack);
    RUN_TEST(test_a_malformed_ack_is_passed_on_but_reports_nothing);
    RUN_TEST(test_the_largest_sequence_number_is_read_in_full);
    RUN_TEST(test_a_sequence_number_too_big_for_32_bits_is_not_an_ack);
    RUN_TEST(test_a_line_of_the_longest_length_is_kept);
    RUN_TEST(test_an_overlong_line_is_dropped_and_the_next_line_is_read);
    RUN_TEST(test_a_line_with_a_non_printable_byte_is_dropped);
    RUN_TEST(test_empty_lines_are_ignored);
    RUN_TEST(test_a_line_that_lost_bytes_is_dropped_and_the_next_line_is_read);
    RUN_TEST(test_bytes_lost_between_lines_drop_the_line_after);
    return TEST_SUMMARY();
}
