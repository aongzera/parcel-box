/*
 * Serial ports (docs/pinmap.md), both 115200 8N1:
 *   huart1, USART1 on PA9 (TX) / PA10 (RX): the link to the ESP32;
 *   huart2, USART2 on PA2 / PA3: the debug log, on the ST-LINK virtual COM
 *   port.
 * Both are set up by MX_USART1_UART_Init() / MX_USART2_UART_Init() in
 * main.c, from parcel-box.ioc. Part of the hardware adapter, so it may use
 * HAL. Sending blocks until the bytes are out (about 2 ms per line per port).
 *
 * Incoming bytes are not read yet: the ESP32's ACK lines come with the
 * Event sender (#27). Both ports' receivers are on (UART_MODE_TX_RX), so
 * any byte that arrives meanwhile sets the overrun flag (ORE). That doesn't
 * affect sending, but #27 must clear it (__HAL_UART_CLEAR_OREFLAG) before
 * it starts receiving, or the first receive reports a stale overrun error.
 */
#ifndef SERIAL_H
#define SERIAL_H

/* Send one protocol line (ending in "\n") to the ESP32, and mirror it to the
 * debug log. The mirror ends in "\r\n" so any serial terminal shows it on
 * its own line. */
void serial_send_line(const char *line);

/* Write text to the debug log only, never to the ESP32. */
void serial_debug(const char *text);

#endif /* SERIAL_H */
