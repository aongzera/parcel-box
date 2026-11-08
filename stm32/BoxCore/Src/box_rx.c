/* UART line reader: see box_rx.h. */
#include "box_rx.h"

#include <string.h>

#include "box_checksum.h"

void box_rx_init(BoxRx *rx, BoxRxOutputs outputs)
{
    memset(rx, 0, sizeof *rx);
    rx->outputs = outputs;
}

/* "ACK," then 1 to 10 digits worth at most UINT32_MAX, and nothing else. */
static bool parse_ack(const char *line, uint32_t *seq)
{
    uint32_t value = 0;
    const char *p = line + 4;
    int digits = 0;

    if (strncmp(line, "ACK,", 4) != 0) {
        return false;
    }
    for (; *p >= '0' && *p <= '9'; p++) {
        uint32_t digit = (uint32_t)(*p - '0');
        if (++digits > 10 || value > (UINT32_MAX - digit) / 10u) {
            return false;   /* more than 32 bits */
        }
        value = value * 10u + digit;
    }
    if (digits == 0 || *p != '\0') {
        return false;
    }
    *seq = value;
    return true;
}

/* Printable ASCII only: no control bytes (a stray "\r" included) and no
 * bytes with the top bit set, as line noise tends to produce. */
static bool all_printable(const char *text, size_t length)
{
    size_t i;
    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20u || c > 0x7eu) {
            return false;
        }
    }
    return true;
}

static void end_line(BoxRx *rx)
{
    uint32_t seq;
    size_t body_length = 0;
    bool empty;
    bool keep;

    if (rx->length > 0u && rx->line[rx->length - 1u] == '\r') {
        rx->length--;   /* a "\r\n" ending */
    }
    empty = rx->length == 0u;   /* skipped silently */
    keep = !rx->spoiled && !empty && rx->length <= BOX_RX_LINE_MAX &&
           all_printable(rx->line, rx->length);
    rx->line[rx->length] = '\0';
    rx->length = 0;
    rx->spoiled = false;
    /* A missing or wrong checksum: maybe garbled, so dropped too. */
    keep = keep && box_checksum_check(rx->line, &body_length);
    if (!keep) {
        if (!empty && rx->outputs.on_dropped != NULL) {
            rx->outputs.on_dropped(rx->outputs.context);
        }
        return;
    }
    /* Mirrored as received, checksum and all; then read without it. */
    if (rx->outputs.on_line != NULL) {
        rx->outputs.on_line(rx->outputs.context, rx->line);
    }
    rx->line[body_length] = '\0';
    if (parse_ack(rx->line, &seq) && rx->outputs.on_ack != NULL) {
        rx->outputs.on_ack(rx->outputs.context, seq);
    }
}

void box_rx_feed(BoxRx *rx, uint8_t byte)
{
    if (byte == '\n') {
        end_line(rx);
    } else if (rx->length < sizeof rx->line - 1u) {
        rx->line[rx->length++] = (char)byte;
    } else {
        rx->spoiled = true;   /* too long, even allowing for a "\r" */
    }
}

void box_rx_lost(BoxRx *rx)
{
    rx->spoiled = true;
}
