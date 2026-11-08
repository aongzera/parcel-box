/*
 * The ESP32-S3 bridge: wires the board link (lib/bridge_link) to the STM32's
 * UART, and to the USB serial monitor so you can type the STM32's lines by
 * hand (product.md section 5). Each source gets its own link, so a half
 * typed line never mixes with the STM32's bytes, and each one's replies go
 * back where its lines came from.
 *
 * On the USB serial monitor:
 *   < line          a line received intact from the STM32
 *   > ACK,n         a reply sent to the STM32
 *   ACK,n           a reply to a line you typed
 *   # dropped line  a line from the STM32, or typed, was too long, held a
 *                   non-printable byte (a backspace, say), or came around
 *                   a UART receive error, so it was ignored
 *   # forward ...   an Event, or a changed Box status, to forward. A resent
 *                   Event (its ACK was lost) is ACKed again but not
 *                   forwarded; type BOOT first to start a fresh session.
 *
 * A UART receive error on the STM32 link drops every line from the STM32
 * until it has been quiet for STM32_QUIET_MS, so a lost or garbled digit
 * can't turn EVT,12 into EVT,1 or EVT,19. The STM32 resends what matters.
 *
 * Forwarding to Firebase and Telegram is a stub for now: it only prints the
 * "# forward ..." line.
 */
#include <Arduino.h>

#include <atomic>

#include "bridge_link.h"

/* UART1 to the STM32, 115200 8N1 (docs/pinmap.md). */
static const int STM32_RX_PIN = 18;   /* from Nucleo D8 (PA9, TX) */
static const int STM32_TX_PIN = 17;   /* to Nucleo D2 (PA10, RX) */
static const unsigned long LINK_BAUD = 115200;

static BridgeLink stm32_link;   /* lines from the STM32 */
static BridgeLink usb_link;     /* lines typed in the serial monitor */

/* After a receive error, how long the STM32 line must have no bytes before
 * lines from it are read again: many byte-times (one is ~87 us), so every
 * byte from around the error has been read by then. The STM32 sends each
 * line in one go, so this never splits a good one. */
static const unsigned long STM32_QUIET_MS = 10;

/* Set by Serial1's receive-error callback (any error type), which runs in
 * the UART event task, not in loop(): loop() passes it on to stm32_link. */
static std::atomic<bool> stm32_receive_error(false);

/* When a byte from the STM32, or a receive error, last came. */
static unsigned long stm32_last_activity_ms;

static void note_stm32_receive_error(hardwareSerial_error_t)
{
    stm32_receive_error = true;
}

static void pass_on_stm32_receive_error()
{
    if (stm32_receive_error.exchange(false)) {
        bridge_link_lost(&stm32_link);
        stm32_last_activity_ms = millis();
    }
}

/* The last Box status printed. Only a change is printed; #23 must still
 * update last-seen on every STS (heartbeat). */
static bool status_known = false;
static BridgeStatus last_status;

static void reply_to_stm32(void *, const char *line)
{
    Serial1.print(line);
    Serial.print("> ");   /* mirror it, to watch the protocol */
    Serial.print(line);
}

static void mirror_stm32_line(void *, const char *line)
{
    Serial.print("< ");
    Serial.println(line);
}

static void reply_to_usb(void *, const char *line)
{
    Serial.print(line);
}

static void report_dropped_line(void *)
{
    Serial.println("# dropped line");
}

static void forward_event(void *, BridgeEvent event)
{
    Serial.printf("# forward EVT,%lu,%s\n", (unsigned long)event.seq,
                  bridge_outcome_name(event.outcome));
}

static void print_status_if_changed(void *, BridgeStatus status)
{
    if (status_known && status.occupied == last_status.occupied &&
        status.lid_open == last_status.lid_open) {
        return;   /* a heartbeat */
    }
    status_known = true;
    last_status = status;
    Serial.printf("# forward STS,%s,%s\n",
                  status.occupied ? "OCCUPIED" : "EMPTY",
                  status.lid_open ? "LID_OPEN" : "LID_CLOSED");
}

void setup()
{
    BridgeLinkOutputs from_stm32 = {};
    BridgeLinkOutputs from_usb = {};

    Serial.begin(LINK_BAUD);
    Serial1.begin(LINK_BAUD, SERIAL_8N1, STM32_RX_PIN, STM32_TX_PIN);
    Serial1.onReceiveError(note_stm32_receive_error);

    from_stm32.write_line = reply_to_stm32;
    from_stm32.on_event = forward_event;
    from_stm32.on_status = print_status_if_changed;
    from_stm32.on_line = mirror_stm32_line;
    from_stm32.on_dropped = report_dropped_line;
    bridge_link_init(&stm32_link, from_stm32);

    from_usb.write_line = reply_to_usb;
    from_usb.on_event = forward_event;
    from_usb.on_status = print_status_if_changed;
    from_usb.on_dropped = report_dropped_line;
    bridge_link_init(&usb_link, from_usb);

    Serial.println("# ESP32 bridge ready");
}

void loop()
{
    /* A receive error is reported after the bytes around it may already be
     * buffered, or partly read, so it doesn't mark the gap's exact place:
     * the link drops every line from here until the line is quiet. That
     * may drop good lines too, which is safe (an EVT or BOOT is resent
     * within 1 s, an STS repeats). It is checked before every byte so as
     * few bytes as possible are read past an error before it is noticed. */
    pass_on_stm32_receive_error();
    while (Serial1.available() > 0) {
        bridge_link_feed(&stm32_link, (uint8_t)Serial1.read());
        stm32_last_activity_ms = millis();
        pass_on_stm32_receive_error();
    }
    pass_on_stm32_receive_error();
    if (millis() - stm32_last_activity_ms >= STM32_QUIET_MS) {
        bridge_link_quiet(&stm32_link);   /* does nothing unless discarding */
    }
    while (Serial.available() > 0) {
        bridge_link_feed(&usb_link, (uint8_t)Serial.read());
    }
}
