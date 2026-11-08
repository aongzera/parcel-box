/*
 * UART line reader: turns the bytes the ESP32 sends into lines of the UART
 * protocol (product.md section 5) and picks out the `ACK,<seq>` lines.
 *
 * Pure C99 with no HAL, like the core, so it is tested on the PC
 * (`make test`). The hardware adapter receives the bytes (in an interrupt)
 * and feeds them in from the main loop, never from the interrupt.
 *
 * How to use it:
 *   1. box_rx_init() with your outputs.
 *   2. box_rx_feed() every received byte, in order.
 *   3. box_rx_lost() wherever bytes were lost (receive buffer full, UART
 *      overrun, framing or noise error), at that point in the byte stream.
 *
 * Every line ends in its checksum, `<body>*HH` (box_checksum.h; upper- or
 * lowercase hex is fine).
 *
 * What comes out:
 *   on_line(line)  every complete line, as received with its checksum but
 *       without its "\r\n" or "\n", for the debug mirror. Unknown lines
 *       come out here too; they are otherwise ignored.
 *   on_ack(seq)    for each well-formed `ACK,<seq>*HH` line, after its
 *       on_line. <seq> is 1 to 10 decimal digits, at most 4294967295, and
 *       only the checksum follows it.
 *   on_dropped()   for each line dropped whole, with no other output: one
 *       longer than BOX_RX_LINE_MAX characters, holding a byte that isn't
 *       printable ASCII, that had bytes lost in it, or with no checksum or
 *       a wrong one (there is no relaxed rule for typed lines here: the
 *       STM32 reads only the ESP32's UART). For the debug mirror.
 * An empty line is skipped silently: no output at all. Reading starts
 * afresh after each "\n".
 */
#ifndef BOX_RX_H
#define BOX_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The longest line kept, not counting its "\r\n". Every line the ESP32 sends
 * (product.md section 5) is far shorter: the longest, "ACK,4294967295*HH",
 * is 17. */
#define BOX_RX_LINE_MAX 32

typedef struct {
    void *context;
    void (*on_line)(void *context, const char *line);   /* may be NULL */
    void (*on_ack)(void *context, uint32_t seq);        /* may be NULL */
    void (*on_dropped)(void *context);                  /* may be NULL */
} BoxRxOutputs;

/* Declared here so it can live in a static variable; treat the fields as
 * private. */
typedef struct {
    BoxRxOutputs outputs;
    char line[BOX_RX_LINE_MAX + 2];   /* room for a "\r" and the NUL */
    size_t length;
    bool spoiled;   /* drop the line in progress when its "\n" comes */
} BoxRx;

void box_rx_init(BoxRx *rx, BoxRxOutputs outputs);

/* One received byte. */
void box_rx_feed(BoxRx *rx, uint8_t byte);

/* Bytes were lost here: the line in progress is dropped. */
void box_rx_lost(BoxRx *rx);

#endif /* BOX_RX_H */
