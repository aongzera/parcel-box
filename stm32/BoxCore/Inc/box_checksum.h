/*
 * Line checksum of the UART protocol (product.md section 5): every line,
 * both ways, is `<body>*HH`, where HH is the CRC-8 of every byte of <body>
 * in two hex digits. The CRC-8 is the polynomial 0x1D (x^8 + x^4 + x^3 +
 * x^2 + 1), initial value 0x00, most significant bit first, no final XOR.
 *
 * 115200 8N1 has no parity, so without a checksum one flipped bit or a
 * garbled digit could turn one valid line into another: EVT,12 into EVT,92
 * ('1' ^ 0x08 is '9'). An XOR would miss the same bit flipped in two
 * different bytes. 0x1D is primitive, so the CRC only repeats after 255
 * bits, and the longest line (248 bits with its CRC) is shorter: one
 * flipped bit is always caught, and so are two inside the body. The hex
 * digits weaken this: one flipped bit in a check character can change the
 * value by more than one bit, so a few pairs of one body bit plus one
 * hex-digit bit get through. The only one that could raise an Event
 * number needs a seq of 100,000,000 or more. Issue #32 will change the
 * encoding so that every 2-bit error is caught.
 *
 * Pure C99 with no HAL. The writers (box_lines, box_sender) seal their
 * lines with box_checksum_append(); the reader (box_rx) checks them with
 * box_checksum_check(). The ESP32 has its own copy of the same rule
 * (bridge_link); the contract test checks that the two agree.
 */
#ifndef BOX_CHECKSUM_H
#define BOX_CHECKSUM_H

#include <stdbool.h>
#include <stddef.h>

/* How many characters box_checksum_append() adds: "*HH\n". */
#define BOX_CHECKSUM_SUFFIX_LEN 4

/* `line` holds a body, NUL-terminated: append "*HH\n" (uppercase hex).
 * Needs room for BOX_CHECKSUM_SUFFIX_LEN more characters. */
void box_checksum_append(char *line);

/* `line` is a received line, NUL-terminated, without its "\n". True if it
 * ends in `*HH` (upper- or lowercase hex) that matches the CRC-8 of every
 * byte before the "*"; then `*body_length` is the length of that body.
 * False for no suffix, a short or non-hex one, or a wrong checksum. */
bool box_checksum_check(const char *line, size_t *body_length);

#endif /* BOX_CHECKSUM_H */
