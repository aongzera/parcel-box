/* ESP32 end of the board link: see bridge_link.h. */
#include "bridge_link.h"

#include <stdio.h>
#include <string.h>

void bridge_link_init(BridgeLink *link, BridgeLinkOutputs outputs)
{
    memset(link, 0, sizeof *link);
    link->outputs = outputs;
}

/* Indexed by BridgeOutcome. */
static const char *const outcome_names[] = {
    "DELIVERED", "COLLECTED", "OPENED_OCCUPIED", "OPENED_EMPTY"
};
#define OUTCOME_COUNT (sizeof outcome_names / sizeof outcome_names[0])

static void reply(BridgeLink *link, const char *line)
{
    if (link->outputs.write_line != NULL) {
        link->outputs.write_line(link->outputs.context, line);
    }
}

/* 1 to 4294967295 in decimal with no leading zero, ended by `end`. Sets
 * `*rest` to the byte after the digits. */
static bool parse_seq(const char *text, char end, uint32_t *seq, const char **rest)
{
    uint32_t value = 0;
    const char *p = text;

    if (*p < '1' || *p > '9') {
        return false;   /* no digits, or a leading zero */
    }
    for (; *p >= '0' && *p <= '9'; p++) {
        uint32_t digit = (uint32_t)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10u) {
            return false;   /* more than 32 bits */
        }
        value = value * 10u + digit;
    }
    if (*p != end) {
        return false;
    }
    *seq = value;
    *rest = p;
    return true;
}

static bool parse_outcome(const char *text, BridgeOutcome *outcome)
{
    size_t i;
    for (i = 0; i < OUTCOME_COUNT; i++) {
        if (strcmp(text, outcome_names[i]) == 0) {
            *outcome = (BridgeOutcome)i;
            return true;
        }
    }
    return false;
}

/* "EVT,<seq>,<OUTCOME>": ACK it, then forward it if it is new in this
 * session. */
static void handle_event(BridgeLink *link, const char *fields)
{
    BridgeEvent event;
    const char *rest;
    char ack[BRIDGE_LINK_REPLY_MAX];

    if (!parse_seq(fields, ',', &event.seq, &rest) ||
        !parse_outcome(rest + 1, &event.outcome)) {
        return;   /* garbled: no ACK, so the STM32 sends it again */
    }
    snprintf(ack, sizeof ack, "ACK,%lu\n", (unsigned long)event.seq);
    reply(link, ack);
    if (event.seq <= link->last_seq) {
        return;   /* a resend of one already accepted */
    }
    link->last_seq = event.seq;
    if (link->outputs.on_event != NULL) {
        link->outputs.on_event(link->outputs.context, event);
    }
}

/* "STS,<EMPTY|OCCUPIED>,<LID_OPEN|LID_CLOSED>": forward it, no reply. */
static void handle_status(BridgeLink *link, const char *fields)
{
    BridgeStatus status;

    if (strncmp(fields, "EMPTY,", 6) == 0) {
        status.occupied = false;
        fields += 6;
    } else if (strncmp(fields, "OCCUPIED,", 9) == 0) {
        status.occupied = true;
        fields += 9;
    } else {
        return;
    }
    if (strcmp(fields, "LID_OPEN") == 0) {
        status.lid_open = true;
    } else if (strcmp(fields, "LID_CLOSED") == 0) {
        status.lid_open = false;
    } else {
        return;
    }
    if (link->outputs.on_status != NULL) {
        link->outputs.on_status(link->outputs.context, status);
    }
}

static void handle_line(BridgeLink *link, const char *line)
{
    if (strcmp(line, "BOOT") == 0) {
        link->last_seq = 0;   /* a new session: nothing accepted in it yet */
        reply(link, "ACK,0\n");
    } else if (strncmp(line, "EVT,", 4) == 0) {
        handle_event(link, line + 4);
    } else if (strncmp(line, "STS,", 4) == 0) {
        handle_status(link, line + 4);
    }
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

static void end_line(BridgeLink *link)
{
    bool keep;
    bool empty;
    bool spoiled = link->spoiled || link->discarding;

    if (link->length > 0u && link->line[link->length - 1u] == '\r') {
        link->length--;   /* a "\r\n" ending */
    }
    empty = !spoiled && link->length == 0u;
    keep = !spoiled && link->length <= BRIDGE_LINK_LINE_MAX &&
           all_printable(link->line, link->length);
    link->line[link->length] = '\0';
    link->length = 0;
    link->spoiled = false;
    if (empty) {
        return;
    }
    if (!keep) {
        if (link->outputs.on_dropped != NULL) {
            link->outputs.on_dropped(link->outputs.context);
        }
        return;
    }
    if (link->outputs.on_line != NULL) {
        link->outputs.on_line(link->outputs.context, link->line);
    }
    handle_line(link, link->line);
}

void bridge_link_feed(BridgeLink *link, uint8_t byte)
{
    if (byte == '\n') {
        end_line(link);
    } else if (link->length < sizeof link->line - 1u) {
        link->line[link->length++] = (char)byte;
    } else {
        link->spoiled = true;   /* too long, even allowing for a "\r" */
    }
}

void bridge_link_lost(BridgeLink *link)
{
    link->discarding = true;
}

void bridge_link_quiet(BridgeLink *link)
{
    if (link->discarding) {
        link->discarding = false;
        if (link->length > 0u) {
            link->spoiled = true;   /* begun while discarding */
        }
    }
}

const char *bridge_outcome_name(BridgeOutcome outcome)
{
    return (size_t)outcome < OUTCOME_COUNT ? outcome_names[outcome] : "";
}
