/*
 * PC tests for the receive ring (box_rx_ring). Each test puts bytes and
 * losses in as the USART1 interrupt would, drains the ring into a real UART
 * line reader (box_rx), and checks only the ACKs that come out.
 */
#include <string.h>

#include "box_rx_ring.h"
#include "test_harness.h"

#define MAX_ACKS 64

typedef struct Rig Rig;

struct Rig {
    BoxRxRing ring;
    BoxRx reader;
    uint32_t acks[MAX_ACKS];
    int ack_count;
    /* Bytes to put in the ring from inside on_line, as the interrupt would
     * while the main loop blocks mirroring that line. */
    const char *arrives_during_line;
};

static void put(Rig *rig, const char *bytes)
{
    while (*bytes != '\0') {
        box_rx_ring_put_byte(&rig->ring, (uint8_t)*bytes++);
    }
}

static void on_line(void *context, const char *line)
{
    Rig *rig = context;
    (void)line;
    if (rig->arrives_during_line != NULL) {
        put(rig, rig->arrives_during_line);
    }
}

static void on_ack(void *context, uint32_t seq)
{
    Rig *rig = context;
    if (rig->ack_count < MAX_ACKS) {
        rig->acks[rig->ack_count] = seq;
    }
    rig->ack_count++;
}

static void start(Rig *rig)
{
    BoxRxOutputs outputs;
    memset(rig, 0, sizeof *rig);
    memset(&outputs, 0, sizeof outputs);
    outputs.context = rig;
    outputs.on_line = on_line;
    outputs.on_ack = on_ack;
    box_rx_init(&rig->reader, outputs);
    box_rx_ring_init(&rig->ring);
}

static void drain(Rig *rig)
{
    box_rx_ring_drain(&rig->ring, &rig->reader);
}

#define CHECK_ACK(rig, index, expected)                                     \
    do {                                                                    \
        CHECK((index) < MAX_ACKS && (rig)->ack_count > (index));            \
        if ((index) < MAX_ACKS && (rig)->ack_count > (index)) {             \
            CHECK_EQ_INT((expected), (rig)->acks[(index)]);                 \
        }                                                                   \
    } while (0)

/* ---- Tests --------------------------------------------------------------- */

static void test_received_bytes_reach_the_reader_in_order(void)
{
    Rig rig;
    start(&rig);

    put(&rig, "ACK,1*6E\r\nACK,2*49\n");
    drain(&rig);

    CHECK_EQ_INT(2, rig.ack_count);
    CHECK_ACK(&rig, 0, 1);
    CHECK_ACK(&rig, 1, 2);
}

static void test_the_first_line_into_a_fresh_ring_is_read(void)
{
    Rig rig;
    start(&rig);

    put(&rig, "ACK,1*6E\n");   /* the ESP32's first reply after a boot */
    drain(&rig);

    CHECK_EQ_INT(1, rig.ack_count);
    CHECK_ACK(&rig, 0, 1);
}

static void test_a_loss_drops_the_line_it_falls_in(void)
{
    Rig rig;
    start(&rig);

    /* A loss in the second "ACK,1*6E": what arrived still checks out, but
     * the line must be dropped all the same. */
    put(&rig, "ACK,1*6E\nACK,1");
    box_rx_ring_note_loss(&rig.ring);
    put(&rig, "*6E\nACK,3*54\n");
    drain(&rig);

    CHECK_EQ_INT(2, rig.ack_count);
    CHECK_ACK(&rig, 0, 1);
    CHECK_ACK(&rig, 1, 3);
}

static void test_a_loss_before_the_first_byte_drops_a_stale_line_tail(void)
{
    Rig rig;
    start(&rig);

    /* Receiving starts mid-line: the line's start went by before, and the
     * tail that is left could even check out as a line of its own. */
    box_rx_ring_note_loss(&rig.ring);
    put(&rig, "ACK,5*1A\nACK,124*17\n");
    drain(&rig);

    CHECK_EQ_INT(1, rig.ack_count);
    CHECK_ACK(&rig, 0, 124);
}

static void test_bytes_that_find_the_ring_full_are_lost_and_their_line_dropped(void)
{
    Rig rig;
    int i;
    start(&rig);

    /* 13 lines of 9 bytes and three empty lines are 120 entries; "ACK,123*"
     * then fills the ring, so the "44" of "ACK,123*44" finds it full. */
    for (i = 0; i < 13; i++) {
        put(&rig, "ACK,1*6E\n");
    }
    put(&rig, "\n\n\nACK,123*44");
    drain(&rig);
    /* The "\n" arrives once there is room again. It must not end the line
     * as "ACK,123*", or anything else. */
    put(&rig, "\nACK,5*1A\n");
    drain(&rig);

    CHECK_EQ_INT(14, rig.ack_count);
    CHECK_ACK(&rig, 0, 1);
    CHECK_ACK(&rig, 12, 1);
    CHECK_ACK(&rig, 13, 5);
}

static void test_a_drain_stops_at_the_bytes_that_were_waiting_when_it_began(void)
{
    Rig rig;
    start(&rig);
    rig.arrives_during_line = "ACK,8*9B\n";

    put(&rig, "ACK,7*20\n");
    drain(&rig);

    CHECK_EQ_INT(1, rig.ack_count);
    CHECK_ACK(&rig, 0, 7);

    rig.arrives_during_line = NULL;
    drain(&rig);

    CHECK_EQ_INT(2, rig.ack_count);
    CHECK_ACK(&rig, 1, 8);
}

int main(void)
{
    RUN_TEST(test_received_bytes_reach_the_reader_in_order);
    RUN_TEST(test_the_first_line_into_a_fresh_ring_is_read);
    RUN_TEST(test_a_loss_drops_the_line_it_falls_in);
    RUN_TEST(test_a_loss_before_the_first_byte_drops_a_stale_line_tail);
    RUN_TEST(test_bytes_that_find_the_ring_full_are_lost_and_their_line_dropped);
    RUN_TEST(test_a_drain_stops_at_the_bytes_that_were_waiting_when_it_began);
    return TEST_SUMMARY();
}
