/*
 * ESP32 end of the board link: reads the lines the STM32 sends over the UART
 * protocol (product.md section 5), replies with the ACKs, and hands each
 * Event and Box status to the code that forwards them to the cloud.
 *
 * Pure C99 with no Arduino calls, so it is tested on the PC (`make test`).
 * main.cpp feeds it the bytes from the STM32's UART, or typed in the USB
 * serial monitor, and supplies the outputs.
 *
 * How to use it:
 *   1. bridge_link_init() with your outputs.
 *   2. bridge_link_feed() every received byte, in order.
 *
 * Lines read (each ends in "\n"; a "\r\n" ending is fine too):
 *   BOOT                          reply `ACK,0`.
 *   EVT,<seq>,<OUTCOME>           reply `ACK,<seq>`, then on_event().
 *       <seq> is 1 to 4294967295 in decimal, with no leading zero (Events
 *       are numbered from 1, and `ACK,0` belongs to BOOT). <OUTCOME> is one
 *       of DELIVERED, COLLECTED, OPENED_OCCUPIED, OPENED_EMPTY.
 *   STS,<EMPTY|OCCUPIED>,<LID_OPEN|LID_CLOSED>   on_status(), no reply.
 * Any other line is ignored, with no reply and no Event or status (on_line,
 * below, still reports it). That includes an EVT line with a bad sequence
 * number or outcome: it is not ACKed, so the STM32 sends it again 1 s
 * later, rather than an ACK for an Event that was never forwarded losing
 * it. A line is also dropped if it is longer than
 * BRIDGE_LINK_LINE_MAX characters or holds a byte that isn't printable
 * ASCII (a backspace typed in the serial monitor included). Reading starts
 * afresh after its "\n". An empty line is skipped silently.
 *
 * Other outputs, for the debug log:
 *   on_line(line)  every line kept (not dropped, not empty), without its
 *       "\r\n" or "\n", before it is handled. Unknown lines come out here
 *       too.
 *   on_dropped()   for each line dropped as too long or not printable.
 *
 * on_event() is called the moment the EVT line arrives, so it is the place
 * to stamp the Event's arrival time.
 *
 * Not done here yet: ignoring a resent Event (#28) and forgetting it on
 * BOOT (#29). Both belong in BridgeLink; the outputs won't change.
 */
#ifndef BRIDGE_LINK_H
#define BRIDGE_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The longest line kept, not counting its "\r\n". The longest the STM32
 * sends is "EVT,4294967295,OPENED_OCCUPIED" (30). */
#define BRIDGE_LINK_LINE_MAX 32

/* Room for the longest reply, "ACK,4294967295\n", and its NUL. */
#define BRIDGE_LINK_REPLY_MAX 16

/* The outcome of a Lid cycle (product.md section 4). */
typedef enum {
    BRIDGE_DELIVERED,         /* Delivery */
    BRIDGE_COLLECTED,         /* Collection */
    BRIDGE_OPENED_OCCUPIED,   /* Opened while occupied */
    BRIDGE_OPENED_EMPTY       /* Opened while empty */
} BridgeOutcome;

typedef struct {
    uint32_t seq;
    BridgeOutcome outcome;
} BridgeEvent;

/* Box status. */
typedef struct {
    bool occupied;   /* false: Empty */
    bool lid_open;
} BridgeStatus;

typedef struct {
    void *context;
    /* A reply to the STM32, ending in "\n" and NUL-terminated. */
    void (*write_line)(void *context, const char *line);    /* may be NULL */
    void (*on_event)(void *context, BridgeEvent event);     /* may be NULL */
    void (*on_status)(void *context, BridgeStatus status);  /* may be NULL */
    void (*on_line)(void *context, const char *line);       /* may be NULL */
    void (*on_dropped)(void *context);                      /* may be NULL */
} BridgeLinkOutputs;

/* Declared here so it can live in a static variable; treat the fields as
 * private. */
typedef struct {
    BridgeLinkOutputs outputs;
    char line[BRIDGE_LINK_LINE_MAX + 2];   /* room for a "\r" and the NUL */
    size_t length;
    bool spoiled;   /* drop the line in progress when its "\n" comes */
} BridgeLink;

void bridge_link_init(BridgeLink *link, BridgeLinkOutputs outputs);

/* One received byte. */
void bridge_link_feed(BridgeLink *link, uint8_t byte);

/* The protocol name of an outcome, e.g. "DELIVERED". */
const char *bridge_outcome_name(BridgeOutcome outcome);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_LINK_H */
