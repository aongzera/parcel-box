/*
 * PC tests for the UART line reader (box_rx). Each test feeds bytes as the
 * ESP32 would send them, then checks only the lines and ACKs that come out.
 *
 * Every line ends in its checksum, `*HH`: the CRC-8 (polynomial 0x1D,
 * initial value 0x00) of every byte before the "*". The checksums here
 * were worked out with an independent Python script, not with the code
 * under test.
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
    int dropped_count;
} Recorder;

static void record_dropped(void *context)
{
    Recorder *r = context;
    r->dropped_count++;
}

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
    memset(&outputs, 0, sizeof outputs);
    outputs.context = &rig->recorder;
    outputs.on_line = record_line;
    outputs.on_ack = record_ack;
    outputs.on_dropped = record_dropped;
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

    feed(&rig, "ACK,7*20\n");

    CHECK_EQ_INT(1, rig.recorder.ack_count);
    CHECK_ACK(&rig, 0, 7);
    CHECK_EQ_INT(1, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "ACK,7*20");   /* as received, for the mirror */
}

static void test_a_crlf_line_ending_is_read_like_a_newline(void)
{
    Rig rig;
    start(&rig);

    feed(&rig, "ACK,12*42\r\nACK,13*5F\r\n");

    CHECK_EQ_INT(2, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "ACK,12*42");
    CHECK_LINE(&rig, 1, "ACK,13*5F");
    CHECK_EQ_INT(2, rig.recorder.ack_count);
    CHECK_ACK(&rig, 0, 12);
    CHECK_ACK(&rig, 1, 13);
}

static void test_a_lowercase_checksum_is_read_like_an_uppercase_one(void)
{
    Rig rig;
    start(&rig);

    feed(&rig, "ACK,8*9b\n");

    CHECK_EQ_INT(1, rig.recorder.ack_count);
    CHECK_ACK(&rig, 0, 8);
}

static void test_a_line_with_a_missing_or_wrong_checksum_is_dropped_whole(void)
{
    static const char *const bad[] = {
        "ACK,7",           /* no checksum */
        "ACK,7*",
        "ACK,7*2",         /* one digit */
        "ACK,7*21",        /* wrong */
        "ACK,7*02",        /* the right digits, swapped */
        "ACK,7*2G",        /* not hex */
        "ACK,7*020",       /* three digits */
        "ACK,7*20 ",       /* something after it */
        "ACK,7 *20",       /* the right checksum for "ACK,7", not "ACK,7 " */
        "ACK,7*20*20",
        "ACK,0",           /* what a person would type by hand */
    };
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Rig rig;
        start(&rig);

        feed(&rig, bad[i]);
        feed(&rig, "\n");

        if (rig.recorder.line_count != 0 || rig.recorder.ack_count != 0) {
            printf("    \"%s\" was not dropped\n", bad[i]);
            test_current_failed = 1;
        }
    }
}

static void test_no_single_flipped_bit_in_an_ack_line_is_read_as_an_ack(void)
{
    static const char good[] = "ACK,123*44";
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

            if (rig.recorder.ack_count != 0) {
                printf("    bit %d of byte %u flipped: read as ACK %lu\n", bit,
                       (unsigned)at, (unsigned long)rig.recorder.acks[0]);
                test_current_failed = 1;
            }
        }
    }
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
                    box_rx_feed(&rig.rx, (uint8_t)line[i]);
                }
                box_rx_feed(&rig.rx, '\n');

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

static void test_an_unknown_line_is_passed_on_but_is_not_an_ack(void)
{
    Rig rig;
    start(&rig);

    feed(&rig, "HELLO,1*B7\nACK*0D\n");

    CHECK_EQ_INT(2, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "HELLO,1*B7");
    CHECK_LINE(&rig, 1, "ACK*0D");
    CHECK_EQ_INT(0, rig.recorder.ack_count);
}

static void test_a_malformed_ack_is_passed_on_but_reports_nothing(void)
{
    /* Each with the right checksum for its body. */
    static const char *const malformed[] = {
        "ACK,*9A",            /* no number */
        "ACK,abc*BC",         /* not a number */
        "ACK,12x*98",         /* junk after the number */
        "ACK,12,3*1D",        /* an extra field */
        "ACK,-1*A6",          /* a sign */
        "ACK,+1*13",
        "ACK, 5*E9",          /* a space */
        "ACK,5 *98",
        "ack,5*A1",           /* wrong case */
        "ACK;5*57",
        " ACK,5*32",
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

    feed(&rig, "ACK,4294967295*20\n");

    CHECK_EQ_INT(1, rig.recorder.ack_count);
    CHECK_ACK(&rig, 0, 4294967295u);
}

static void test_a_sequence_number_too_big_for_32_bits_is_not_an_ack(void)
{
    static const char *const too_big[] = {
        "ACK,4294967296*07",      /* UINT32_MAX + 1 */
        "ACK,4294967300*D0",      /* overflows on the last digit */
        "ACK,9999999999*66",
        "ACK,12345678901*55",     /* 11 digits */
        "ACK,00000000001*2B",     /* 11 digits, even if the value is small */
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

static void test_a_line_of_the_longest_length_is_kept(void)
{
    char longest[BOX_RX_LINE_MAX + 1];
    Rig rig;
    start(&rig);

    feed(&rig, x_line(longest, BOX_RX_LINE_MAX));
    feed(&rig, "\r\n");

    CHECK_EQ_INT(1, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, longest);
}

static void test_an_overlong_line_is_dropped_and_the_next_line_is_read(void)
{
    char filler[200];
    Rig rig;
    start(&rig);

    /* One character too many (with a right checksum), then far too many. */
    feed(&rig, x_line(filler, BOX_RX_LINE_MAX + 1));
    feed(&rig, "\n");
    feed(&rig, repeat(filler, '7', 199));
    feed(&rig, "ACK,2*49\n");
    feed(&rig, "ACK,3*54\n");

    CHECK_EQ_INT(1, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "ACK,3*54");
    CHECK_EQ_INT(1, rig.recorder.ack_count);
    CHECK_ACK(&rig, 0, 3);
}

static void test_a_line_with_a_non_printable_byte_is_dropped(void)
{
    Rig rig;
    start(&rig);

    /* Noise as the wire is plugged in, a control byte, a stray "\r", a
     * byte with the top bit set, and a NUL: each spoils its own line only,
     * even with a checksum that counts the bad byte. */
    feed(&rig, "\x01\x7f" "ACK,1*9A\n");
    feed(&rig, "ACK,\r2*F4\n");
    feed(&rig, "ACK,\xb3*72\n");
    box_rx_feed(&rig.rx, 'A');
    box_rx_feed(&rig.rx, 0x00);
    feed(&rig, "CK,4*E4\n");
    feed(&rig, "ACK,5*1A\n");

    CHECK_EQ_INT(1, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "ACK,5*1A");
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

    /* A loss reported in "ACK,12*42" drops it, even though what arrived
     * checks out. */
    feed(&rig, "ACK,12");
    box_rx_lost(&rig.rx);
    feed(&rig, "*42\nACK,124*17\n");

    CHECK_EQ_INT(1, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "ACK,124*17");
    CHECK_EQ_INT(1, rig.recorder.ack_count);
    CHECK_ACK(&rig, 0, 124);
}

static void test_bytes_lost_between_lines_drop_the_line_after(void)
{
    Rig rig;
    start(&rig);

    /* The lost bytes may have held that line's start, or the "\n" before it. */
    feed(&rig, "ACK,1*6E\n");
    box_rx_lost(&rig.rx);
    feed(&rig, "ACK,2*49\nACK,3*54\n");

    CHECK_EQ_INT(2, rig.recorder.ack_count);
    CHECK_ACK(&rig, 0, 1);
    CHECK_ACK(&rig, 1, 3);
}

static void test_each_dropped_line_is_reported_once(void)
{
    char overlong[BOX_RX_LINE_MAX + 2];
    Rig rig;
    start(&rig);

    feed(&rig, "ACK,7*21\n");                          /* a wrong checksum */
    feed(&rig, "ACK,7\n");                             /* none at all */
    feed(&rig, x_line(overlong, BOX_RX_LINE_MAX + 1));
    feed(&rig, "\n");                                  /* too long */
    feed(&rig, "ACK,\xb3*72\n");                       /* not printable */
    feed(&rig, "ACK,12");
    box_rx_lost(&rig.rx);
    feed(&rig, "*42\n");                               /* lost bytes */
    feed(&rig, "\n\r\n");                              /* empty: not reported */
    feed(&rig, "ACK,5*1A\n");

    CHECK_EQ_INT(5, rig.recorder.dropped_count);
    CHECK_EQ_INT(1, rig.recorder.line_count);
    CHECK_LINE(&rig, 0, "ACK,5*1A");
    CHECK_EQ_INT(1, rig.recorder.ack_count);
}

static void test_a_line_that_is_kept_is_not_reported_as_dropped(void)
{
    Rig rig;
    start(&rig);

    feed(&rig, "ACK,1*6E\nHELLO,1*B7\n");

    CHECK_EQ_INT(0, rig.recorder.dropped_count);
    CHECK_EQ_INT(2, rig.recorder.line_count);
}

static void test_with_only_on_ack_set_dropped_lines_are_still_ignored(void)
{
    BoxRx rx;
    BoxRxOutputs outputs;
    Recorder recorder;
    const char *bytes = "ACK,7*21\nACK,7\nACK,\xb3*72\nACK,5*1A\n";

    memset(&recorder, 0, sizeof recorder);
    memset(&outputs, 0, sizeof outputs);
    outputs.context = &recorder;
    outputs.on_ack = record_ack;
    box_rx_init(&rx, outputs);
    while (*bytes != '\0') {
        box_rx_feed(&rx, (uint8_t)*bytes++);
    }
    box_rx_lost(&rx);
    box_rx_feed(&rx, '\n');

    CHECK_EQ_INT(1, recorder.ack_count);
    CHECK_EQ_INT(5, (int)recorder.acks[0]);
}

int main(void)
{
    RUN_TEST(test_an_ack_line_reports_its_sequence_number);
    RUN_TEST(test_a_crlf_line_ending_is_read_like_a_newline);
    RUN_TEST(test_a_lowercase_checksum_is_read_like_an_uppercase_one);
    RUN_TEST(test_a_line_with_a_missing_or_wrong_checksum_is_dropped_whole);
    RUN_TEST(test_no_single_flipped_bit_in_an_ack_line_is_read_as_an_ack);
    RUN_TEST(test_no_two_flipped_bits_in_the_longest_line_of_each_kind_are_read_as_another_line);
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
    RUN_TEST(test_each_dropped_line_is_reported_once);
    RUN_TEST(test_a_line_that_is_kept_is_not_reported_as_dropped);
    RUN_TEST(test_with_only_on_ack_set_dropped_lines_are_still_ignored);
    return TEST_SUMMARY();
}
