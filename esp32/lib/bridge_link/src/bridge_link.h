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
 *   1. bridge_link_init() with your outputs. For the USB link only (lines a
 *      person types), also bridge_link_allow_lines_without_checksum().
 *   2. bridge_link_feed() every received byte, in order.
 *   3. bridge_link_lost() on any UART receive error (an overrun loses
 *      bytes; a framing, parity or break error garbles them). It need not
 *      come at the exact point in the byte stream: every line is dropped
 *      until bridge_link_quiet().
 *   4. bridge_link_quiet() once the line has had no bytes for a few
 *      byte-times (main.cpp waits 10 ms), whether or not bytes were lost.
 *
 * Dropping good lines is safe, a kept garbled one is not: an EVT or BOOT is
 * resent within 1 s and STS repeats, but a garbled EVT that read as a
 * higher number would make the real Events below it look like resends.
 *
 * Checksums: every line, both ways, is `<body>*HH`, where HH is the CRC-8
 * of every byte of <body> in two hex digits (product.md section 5): the
 * polynomial 0x1D (x^8 + x^4 + x^3 + x^2 + 1), initial value 0x00, most
 * significant bit first, no final XOR. 115200 8N1 has no parity, so
 * without it one flipped bit could turn EVT,12 into EVT,92 ('1' ^ 0x08 is
 * '9') and make Events 13 to 92 look like resends. An XOR would miss the
 * same bit flipped in two bytes. 0x1D is primitive, so the CRC only
 * repeats after 255 bits, and the longest line (248 bits with its CRC) is
 * shorter: one flipped bit is always caught, and so are two inside the
 * body. The hex digits weaken this: one flipped bit in a check character
 * can change the value by more than one bit, so a few pairs of one body
 * bit plus one hex-digit bit get through. The only one that could raise
 * an Event number needs a seq of 100,000,000 or more. Issue #32 will
 * change the encoding so that every 2-bit error is caught. A line is kept only if it ends in a "*" and two hex digits that
 * match the CRC-8 of every byte before that "*". One with no checksum, or
 * a wrong or malformed one, is dropped like a garbled line: no reply, and
 * on_dropped(). Upper- and lowercase hex are both read; replies are always
 * sent in uppercase. After bridge_link_allow_lines_without_checksum(), a
 * line with no "*" at all is read as a whole body too, so a person can
 * type `BOOT` or `EVT,1,DELIVERED`; a line with a "*" must still check
 * out. Replies always carry a checksum.
 *
 * Lines read, shown without their checksums (each ends in "\n"; a "\r\n"
 * ending is fine too):
 *   BOOT                          reply `ACK,0` and start a new session.
 *   EVT,<seq>,<OUTCOME>           reply `ACK,<seq>`, then on_event() if
 *       the Event is new (see Sessions, below).
 *       <seq> is 1 to 4294967295 in decimal, with no leading zero (Events
 *       are numbered from 1, and `ACK,0` belongs to BOOT). <OUTCOME> is one
 *       of DELIVERED, COLLECTED, OPENED_OCCUPIED, OPENED_EMPTY.
 *   STS,<EMPTY|OCCUPIED>,<LID_OPEN|LID_CLOSED>   on_status(), no reply.
 * Any other line is ignored, with no reply and no Event or status (on_line,
 * below, still reports it). That includes an EVT line with a bad sequence
 * number or outcome: it is not ACKed, so the STM32 sends it again 1 s
 * later, rather than an ACK for an Event that was never forwarded losing
 * it. A line is also dropped if it has a missing or wrong checksum (see
 * Checksums, above), is longer than BRIDGE_LINK_LINE_MAX characters, holds
 * a byte that isn't printable ASCII (a backspace typed in the serial
 * monitor included), or ends between bridge_link_lost() and
 * bridge_link_quiet(), or was begun before that quiet. Reading starts
 * afresh after its "\n". An empty line is skipped silently.
 *
 * Sessions (product.md section 5, "Sessions and duplicates"): an EVT is
 * accepted, and passed to on_event(), only if its <seq> is higher than the
 * last one accepted in this session. Otherwise it is a resend of one
 * already accepted (its ACK was lost): it is ACKed again but not passed
 * on, so each Event is forwarded once. Gaps in the numbers are normal.
 * Every BOOT starts a new session, as does bridge_link_init(), so the
 * first EVT after either is accepted whatever its number.
 *
 * Other outputs, for the debug log:
 *   on_line(line)  every line kept (not dropped, not empty), as received
 *       with its checksum but without its "\r\n" or "\n", before it is
 *       handled. Unknown lines come out here too.
 *   on_dropped()   for each line dropped as too long, not printable,
 *       around lost bytes, or for its checksum.
 *
 * on_event() is called the moment the EVT line arrives, so it is the place
 * to stamp the Event's arrival time.
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
 * sends is "EVT,4294967295,OPENED_OCCUPIED*HH" (33). */
#define BRIDGE_LINK_LINE_MAX 36

/* Room for the longest reply, "ACK,4294967295*HH\n" (18), and its NUL. */
#define BRIDGE_LINK_REPLY_MAX 20

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
    bool discarding;   /* bytes were lost: drop every line until quiet */
    bool allow_unchecked;   /* a line with no "*" needs no checksum */
    uint32_t last_seq;   /* last Event accepted this session; 0: none yet */
} BridgeLink;

/* Every line must carry a right checksum, as on the STM32's link. */
void bridge_link_init(BridgeLink *link, BridgeLinkOutputs outputs);

/* For the USB link only, never the STM32's: from now on a line with no "*"
 * at all is read without a checksum, so a person can type lines by hand.
 * A line with a "*" must still check out. Call it after bridge_link_init(). */
void bridge_link_allow_lines_without_checksum(BridgeLink *link);

/* One received byte. */
void bridge_link_feed(BridgeLink *link, uint8_t byte);

/* Bytes may have been lost or garbled: the line in progress, and every
 * line after it, is dropped until bridge_link_quiet(). */
void bridge_link_lost(BridgeLink *link);

/* The line has been quiet (no bytes) for a few byte-times, so every byte
 * from around the loss has been read: lines are read again, starting with
 * the next one. A line begun before now is still dropped. Does nothing if
 * no bytes were lost, so it may be called on every quiet spell. */
void bridge_link_quiet(BridgeLink *link);

/* The protocol name of an outcome, e.g. "DELIVERED". */
const char *bridge_outcome_name(BridgeOutcome outcome);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_LINK_H */
