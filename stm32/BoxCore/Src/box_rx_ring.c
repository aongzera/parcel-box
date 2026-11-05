/* Receive ring: see box_rx_ring.h. */
#include "box_rx_ring.h"

#include <string.h>

/* C99 has no static_assert: the array size is -1, a compile error, unless
 * BOX_RX_RING_SIZE is a power of two (needed for the index mask below). */
typedef char box_rx_ring_size_must_be_a_power_of_two
    [(BOX_RX_RING_SIZE & (BOX_RX_RING_SIZE - 1u)) == 0u ? 1 : -1];

#define LOSS_MARKER 0x100u   /* not a byte value */

void box_rx_ring_init(BoxRxRing *ring)
{
    memset((void *)ring, 0, sizeof *ring);
}

static bool put(BoxRxRing *ring, uint16_t entry)
{
    uint32_t head = ring->head;
    if (head - ring->tail >= BOX_RX_RING_SIZE) {
        return false;
    }
    ring->entries[head & (BOX_RX_RING_SIZE - 1u)] = entry;
    ring->head = head + 1u;   /* publish only after the entry is written */
    return true;
}

void box_rx_ring_note_loss(BoxRxRing *ring)
{
    ring->loss_pending = !put(ring, LOSS_MARKER);
}

void box_rx_ring_put_byte(BoxRxRing *ring, uint8_t byte)
{
    if (ring->loss_pending) {
        box_rx_ring_note_loss(ring);   /* try again to put the marker in */
    }
    /* Checked again rather than trying the byte anyway: the main loop may
     * free a slot after the marker didn't fit, and the byte must never go
     * in without the marker in front of it. */
    if (ring->loss_pending || !put(ring, byte)) {
        ring->loss_pending = true;     /* this byte is lost too */
    }
}

void box_rx_ring_drain(BoxRxRing *ring, BoxRx *reader)
{
    uint32_t tail = ring->tail;
    const uint32_t head = ring->head;   /* only what is waiting now */
    while (tail != head) {
        uint16_t entry = ring->entries[tail & (BOX_RX_RING_SIZE - 1u)];
        tail++;
        ring->tail = tail;   /* frees the slot for the interrupt */
        if (entry == LOSS_MARKER) {
            box_rx_lost(reader);
        } else {
            box_rx_feed(reader, (uint8_t)entry);
        }
    }
}
