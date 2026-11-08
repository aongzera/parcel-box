/*
 * The ESP32-S3 bridge: wires the board link (lib/bridge_link) to the STM32's
 * UART, and to the USB serial monitor so you can type the STM32's lines by
 * hand (product.md section 5). Each source gets its own link, so a half
 * typed line never mixes with the STM32's bytes, and each one's replies go
 * back where its lines came from.
 *
 * Every line on the STM32 link, both ways, ends in a checksum, `*HH`
 * (product.md section 5). Lines you type need none: type `BOOT` or
 * `EVT,1,DELIVERED` as they are. A typed line with a "*" must have the
 * right checksum, as the STM32's lines must.
 *
 * On the USB serial monitor:
 *   < line*HH       a line received intact from the STM32
 *   > ACK,n*HH      a reply sent to the STM32
 *   ACK,n*HH        a reply to a line you typed
 *   # dropped line  a line from the STM32, or typed, was too long, held a
 *                   non-printable byte (a backspace, say), came around
 *                   a UART receive error, or had a missing (STM32 only)
 *                   or wrong checksum, so it was ignored
 *   # queued EVT,...   an Event, stamped with the time it arrived and put
 *                   in the offline queue (lib/event_queue). A resent Event
 *                   (its ACK was lost) is ACKed again but not queued; type
 *                   BOOT first to start a fresh session.
 *   # upload EVT,<seq>,<OUTCOME> at <time | unknown>
 *                   an Event leaving the queue, oldest first. Only while
 *                   WiFi is connected; otherwise it waits in the queue.
 *   # Queue full: dropped Event <seq>
 *                   the queue was full, so its oldest Event was dropped
 *   # forward STS,...  a changed Box status, to forward
 *   # WiFi connected / # WiFi disconnected
 *
 * A UART receive error on the STM32 link drops every line from the STM32
 * until it has been quiet for STM32_QUIET_MS, and the checksum catches the
 * garbling a UART can't see (8N1 has no parity), so a lost digit or a
 * flipped bit can't turn EVT,12 into EVT,1 or EVT,92. The STM32 resends
 * what matters.
 *
 * WiFi and NTP start in setup() without waiting for either, and reconnect
 * by themselves. The ACKs never wait for them: bridge_link ACKs each EVT
 * before handing it on, and the queue never talks to the STM32, so the
 * STM32's outbox keeps moving while the ESP32 is offline.
 *
 * The upload is a stub until #23 (Firebase): it only prints the
 * "# upload ..." line, and succeeds only while WiFi is connected. Box
 * status forwarding is a stub too: it only prints "# forward STS,...".
 */
#include <Arduino.h>
#include <WiFi.h>

#include <atomic>
#include <time.h>

#include "bridge_link.h"
#include "event_queue.h"
#include "event_time.h"
#include "secrets.h"

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

/* Events from both links, waiting to be uploaded (lib/event_queue). */
static EventQueue event_queue;

/* The link calls this the moment an EVT line arrives, after it has ACKed
 * it, so the queue stamps the Event with its arrival time. */
static void queue_event(void *, BridgeEvent event)
{
    Serial.printf("# queued EVT,%lu,%s\n", (unsigned long)event.seq,
                  bridge_outcome_name(event.outcome));
    event_queue_push(&event_queue, event);
}

/* Seconds since 1970 once NTP has synced; before that, since boot. */
static int64_t read_clock(void *)
{
    return (int64_t)time(nullptr);
}

/* A stub until #23. The real Firebase write goes here: one Event per call,
 * returning true only once it is stored. It runs inside loop(), so keep it
 * short (a timeout well under a second) or make it asynchronous; see the
 * note in event_queue.h. */
static bool upload_event(void *, const QueuedEvent *queued)
{
    char time_text[EVENT_TIME_TEXT_MAX];

    if (WiFi.status() != WL_CONNECTED) {
        return false;   /* stays in the queue; retried later */
    }
    event_time_format(queued->time, time_text);
    Serial.printf("# upload EVT,%lu,%s at %s\n", (unsigned long)queued->event.seq,
                  bridge_outcome_name(queued->event.outcome), time_text);
    return true;
}

static void report_queue_full(void *, const QueuedEvent *dropped)
{
    Serial.printf("# Queue full: dropped Event %lu\n",
                  (unsigned long)dropped->event.seq);
}

/* Print each change of the WiFi connection, for the debug log. */
static bool wifi_was_connected = false;

static void report_wifi_changes()
{
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != wifi_was_connected) {
        wifi_was_connected = connected;
        Serial.println(connected ? "# WiFi connected" : "# WiFi disconnected");
        if (connected) {
            event_queue_retry_now(&event_queue);   /* drain now, not in 5 s */
        }
    }
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
    EventQueueOutputs queue_outputs = {};

    Serial.begin(LINK_BAUD);
    Serial1.begin(LINK_BAUD, SERIAL_8N1, STM32_RX_PIN, STM32_TX_PIN);
    Serial1.onReceiveError(note_stm32_receive_error);

    /* Neither waits: WiFi connects, and NTP syncs, in the background. Until
     * NTP syncs, Events are stamped "unknown" (lib/event_queue/event_time.h). */
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    configTime(0, 0, "pool.ntp.org", "time.google.com");   /* UTC */

    queue_outputs.clock_s = read_clock;
    queue_outputs.upload = upload_event;
    queue_outputs.on_dropped = report_queue_full;
    event_queue_init(&event_queue, queue_outputs);

    from_stm32.write_line = reply_to_stm32;
    from_stm32.on_event = queue_event;
    from_stm32.on_status = print_status_if_changed;
    from_stm32.on_line = mirror_stm32_line;
    from_stm32.on_dropped = report_dropped_line;
    bridge_link_init(&stm32_link, from_stm32);

    from_usb.write_line = reply_to_usb;
    from_usb.on_event = queue_event;   /* typed Events join the same queue */
    from_usb.on_status = print_status_if_changed;
    from_usb.on_dropped = report_dropped_line;
    bridge_link_init(&usb_link, from_usb);
    /* Typed lines only: the STM32 link always needs the checksum. */
    bridge_link_allow_lines_without_checksum(&usb_link);

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
    report_wifi_changes();
    /* At most one upload per pass, so the UART is read again soon. */
    event_queue_tick(&event_queue, millis());
}
