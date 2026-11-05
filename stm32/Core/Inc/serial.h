/*
 * Serial ports (docs/pinmap.md), both 115200 8N1:
 *   huart1, USART1 on PA9 (TX) / PA10 (RX): the link to the ESP32;
 *   huart2, USART2 on PA2 / PA3: the debug log, on the ST-LINK virtual COM
 *   port.
 * Both are set up by MX_USART1_UART_Init() / MX_USART2_UART_Init() in
 * main.c, from parcel-box.ioc. Part of the hardware adapter, so it may use
 * HAL. Sending blocks until the bytes are out (about 2 ms per line per port).
 *
 * Receiving (ESP32 to STM32 only; the debug port's input is never read):
 * the USART1 interrupt (enabled in parcel-box.ioc) takes one byte at a time
 * into the receive ring (box_rx_ring). serial_poll_received(), from the
 * main loop, hands them to the UART line reader (box_rx), never from the
 * interrupt. Where bytes were lost (the ring was full, or a UART overrun,
 * framing, noise or parity error), the reader is told at that point in the
 * stream, so it drops the line they belonged to instead of reading a wrong
 * ACK. If bytes came in before serial_start_receiving() (they are thrown
 * away), the line they began is dropped the same way. Normally the ESP32 is
 * idle at boot, and its first line is read.
 */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

#include "box_rx.h"

/* Send one protocol line (ending in "\n") to the ESP32, and mirror it to the
 * debug log. The mirror ends in "\r\n" so any serial terminal shows it on
 * its own line. */
void serial_send_line(const char *line);

/* Mirror one line received from the ESP32 (without its "\n") to the debug
 * log, as "< " and the line, so it reads apart from the lines sent. */
void serial_mirror_received(const char *line);

/* Write text to the debug log only, never to the ESP32. */
void serial_debug(const char *text);

/* Write `value` in decimal to the debug log only. */
void serial_debug_uint(uint32_t value);

/* Start receiving from the ESP32. Call once, after MX_USART1_UART_Init().
 * Clears the overrun flag that bytes arriving since reset have set. */
void serial_start_receiving(void);

/* Feed the bytes received before this call, and the losses among them, to
 * `reader`, in order. Bytes that arrive meanwhile wait for the next call.
 * Call from the main loop, not from an interrupt. */
void serial_poll_received(BoxRx *reader);

#endif /* SERIAL_H */
