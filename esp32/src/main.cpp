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
 *   # dropped line  a line you typed was too long or held a non-printable
 *                   byte (a backspace, say), so it was ignored
 *   # forward ...   an Event, or a changed Box status, to forward
 *
 * Forwarding to Firebase and Telegram is a stub for now: it only prints the
 * "# forward ..." line.
 */
#include <Arduino.h>

#include "bridge_link.h"

/* UART1 to the STM32, 115200 8N1 (docs/pinmap.md). */
static const int STM32_RX_PIN = 18;   /* from Nucleo D8 (PA9, TX) */
static const int STM32_TX_PIN = 17;   /* to Nucleo D2 (PA10, RX) */
static const unsigned long LINK_BAUD = 115200;

static BridgeLink stm32_link;   /* lines from the STM32 */
static BridgeLink usb_link;     /* lines typed in the serial monitor */

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

static void report_dropped_usb_line(void *)
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

    from_stm32.write_line = reply_to_stm32;
    from_stm32.on_event = forward_event;
    from_stm32.on_status = print_status_if_changed;
    from_stm32.on_line = mirror_stm32_line;
    bridge_link_init(&stm32_link, from_stm32);

    from_usb.write_line = reply_to_usb;
    from_usb.on_event = forward_event;
    from_usb.on_status = print_status_if_changed;
    from_usb.on_dropped = report_dropped_usb_line;
    bridge_link_init(&usb_link, from_usb);

    Serial.println("# ESP32 bridge ready");
}

void loop()
{
    while (Serial1.available() > 0) {
        bridge_link_feed(&stm32_link, (uint8_t)Serial1.read());
    }
    while (Serial.available() > 0) {
        bridge_link_feed(&usb_link, (uint8_t)Serial.read());
    }
}
