/* Serial ports: see serial.h. */
#include "serial.h"

#include <string.h>

#include "main.h"

#define SEND_TIMEOUT_MS 50u    /* a 40-byte line takes ~3.5 ms */

/* Defined in main.c by CubeMX. */
extern UART_HandleTypeDef huart1;  /* ESP32 */
extern UART_HandleTypeDef huart2;  /* debug log */

static void send(UART_HandleTypeDef *uart, const char *bytes, size_t length)
{
    if (length > 0u) {
        /* A timeout only loses this line; the next STS heartbeat repeats
         * the Box status. */
        (void)HAL_UART_Transmit(uart, (const uint8_t *)bytes, (uint16_t)length,
                                SEND_TIMEOUT_MS);
    }
}

void serial_send_line(const char *line)
{
    size_t length = strlen(line);
    send(&huart1, line, length);

    if (length > 0u && line[length - 1u] == '\n') {
        length--;
    }
    send(&huart2, line, length);
    send(&huart2, "\r\n", 2u);
}

void serial_debug(const char *text)
{
    send(&huart2, text, strlen(text));
}
