/* Serial ports: see serial.h. */
#include "serial.h"

#include <stdbool.h>
#include <string.h>

#include "box_rx_ring.h"
#include "main.h"

#define SEND_TIMEOUT_MS 50u    /* a 40-byte line takes ~3.5 ms */

/* Defined in main.c by CubeMX. */
extern UART_HandleTypeDef huart1;  /* ESP32 */
extern UART_HandleTypeDef huart2;  /* debug log */

/* Received bytes waiting for the main loop: the USART1 interrupt puts them
 * in, serial_poll_received() takes them out. */
static BoxRxRing rx_ring;
static uint8_t rx_byte;        /* HAL receives each byte here */

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

void serial_mirror_received(const char *line)
{
    send(&huart2, "< ", 2u);
    send(&huart2, line, strlen(line));
    send(&huart2, "\r\n", 2u);
}

void serial_debug(const char *text)
{
    send(&huart2, text, strlen(text));
}

void serial_debug_uint(uint32_t value)
{
    char digits[10];
    size_t n = sizeof digits;
    do {
        digits[--n] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    send(&huart2, &digits[n], sizeof digits - n);
}

/* ---- Receiving (interrupt side) ------------------------------------------ */

static void receive_next_byte(void)
{
    (void)HAL_UART_Receive_IT(&huart1, &rx_byte, 1u);
}

void serial_start_receiving(void)
{
    /* The receiver has been on since MX_USART1_UART_Init(). RXNE or ORE set
     * means bytes came in since, and are thrown away below. */
    uint32_t status = huart1.Instance->SR;
    bool bytes_came_early = (status & (USART_SR_RXNE | USART_SR_ORE)) != 0u;

    box_rx_ring_init(&rx_ring);
    /* Clear the overrun flag (reading SR then DR), or the first receive
     * reports a stale overrun. Nothing is received yet, so the byte this
     * discards is one of the early ones. */
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    /* Only if bytes were thrown away: the line under way is broken, so the
     * reader drops it. Normally the ESP32 is idle at boot, so this isn't
     * done, and its first line (ACK,1) is read. */
    if (bytes_came_early) {
        box_rx_ring_note_loss(&rx_ring);
    }
    receive_next_byte();
}

/*
 * How HAL's USART1 interrupt (HAL_UART_IRQHandler) reaches the callbacks
 * below, for 1-byte receives:
 *   - A byte with no error: HAL_UART_RxCpltCallback() keeps it.
 *   - A byte with a framing, noise, parity or overrun error (RXNE set): HAL
 *     sets huart->ErrorCode, reads the byte and calls RxCplt, which drops
 *     it and starts the next receive (that clears ErrorCode, so HAL keeps
 *     receiving). Then HAL calls HAL_UART_ErrorCallback(), which records
 *     the loss.
 *   - An overrun with no byte to read (RXNE clear): HAL stops receiving
 *     and calls only HAL_UART_ErrorCallback(), which clears the overrun,
 *     records the loss and starts receiving again.
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart != &huart1) {
        return;
    }
    if (huart->ErrorCode == HAL_UART_ERROR_NONE) {
        box_rx_ring_put_byte(&rx_ring, rx_byte);
    }
    receive_next_byte();
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    uint32_t status;

    if (huart != &huart1) {
        return;
    }
    /* Clear the overrun flag only if HAL couldn't: it is cleared by reading
     * SR then DR, which HAL did if there was a byte (RXNE). Reading DR when
     * RXNE is set would throw away the next good byte. Left set with RXNE
     * clear, it would raise this interrupt again and again. */
    status = huart->Instance->SR;
    if ((status & USART_SR_ORE) != 0u && (status & USART_SR_RXNE) == 0u) {
        __HAL_UART_CLEAR_OREFLAG(huart);
    }
    /* After the clear, so a byte that arrived during it is behind the
     * marker and its line is dropped too. */
    box_rx_ring_note_loss(&rx_ring);
    /* Only after an overrun with RXNE clear has HAL stopped receiving. */
    if (huart->RxState == HAL_UART_STATE_READY) {
        receive_next_byte();
    }
}

/* ---- Receiving (main loop side) ------------------------------------------ */

void serial_poll_received(BoxRx *reader)
{
    /* Only the bytes waiting now: mirroring each line to USART2 sends ~4x
     * as many bytes as the ESP32 sent us, so draining until empty could
     * keep the main loop here (no ticks, no Events, no buzzer). */
    box_rx_ring_drain(&rx_ring, reader);
}
