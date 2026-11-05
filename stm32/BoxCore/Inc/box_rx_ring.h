/*
 * Receive ring: holds the bytes the USART1 interrupt receives until the main
 * loop feeds them to the UART line reader (box_rx.h).
 *
 * Pure C99 with no HAL, so it is tested on the PC (`make test`). One writer
 * and one reader, so no locking: the interrupt only calls the _put_byte and
 * _note_loss functions, and the main loop only calls box_rx_ring_drain().
 *
 * Losses travel in the stream: box_rx_ring_note_loss() puts a marker at
 * that point, and draining it calls box_rx_lost(), so the reader drops the
 * line it fell in. A byte that finds the ring full is lost too. Its marker
 * goes in ahead of the next byte that fits, because the line it belonged to
 * may continue into that byte.
 */
#ifndef BOX_RX_RING_H
#define BOX_RX_RING_H

#include <stdbool.h>
#include <stdint.h>

#include "box_rx.h"

/* 128 entries is ~11 ms of bytes at 115200 baud, longer than the main loop
 * ever goes between drains. Must be a power of two. */
#define BOX_RX_RING_SIZE 128u

/* Declared here so it can live in a static variable; treat the fields as
 * private. Each entry is a byte, or a loss marker. head and tail run freely
 * and wrap; head - tail is the number of entries waiting. */
typedef struct {
    volatile uint16_t entries[BOX_RX_RING_SIZE];
    volatile uint32_t head;   /* written by the interrupt only */
    volatile uint32_t tail;   /* written by the main loop only */
    bool loss_pending;        /* interrupt only: a loss not yet in the ring */
} BoxRxRing;

void box_rx_ring_init(BoxRxRing *ring);

/* Interrupt side: one received byte. */
void box_rx_ring_put_byte(BoxRxRing *ring, uint8_t byte);

/* Interrupt side: bytes were lost here. */
void box_rx_ring_note_loss(BoxRxRing *ring);

/* Main loop side: feed `reader` the entries that were waiting when this was
 * called, in order. Entries the interrupt adds meanwhile (while the reader's
 * outputs block, say, mirroring a line) wait for the next call, so a steady
 * stream of input can't keep the main loop here. */
void box_rx_ring_drain(BoxRxRing *ring, BoxRx *reader);

#endif /* BOX_RX_RING_H */
