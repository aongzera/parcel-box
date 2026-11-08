/*
 * UART line writer: turns what the Box logic core decides into the text
 * lines of the UART protocol (product.md section 5), and schedules the STS
 * heartbeat.
 *
 * Pure C99 with no HAL, like the core, so it is tested on the PC
 * (`make test`). The hardware adapter supplies `write_line`, which sends the
 * bytes to the ESP32 (and mirrors them to the debug port).
 *
 * How to use it:
 *   1. box_lines_init() with your writer.
 *   2. Pass box_lines_core_outputs() to box_core_init(), so every Box
 *      status change becomes a line. Events go to the Event sender
 *      (box_sender.h), which numbers them and writes their EVT lines with
 *      box_lines_format_event().
 *   3. Call box_lines_tick() as often as box_core_tick() (e.g. every 10 ms)
 *      for the heartbeat.
 *
 * Lines written:
 *   STS,<EMPTY|OCCUPIED>,<LID_OPEN|LID_CLOSED>*HH  on every Box status
 *       change, and again every BOX_STS_HEARTBEAT_MS after the last STS. No
 *       STS at all, not even a heartbeat, until the first Box status change.
 * Each line passed to write_line ends in its checksum (box_checksum.h) and
 * "\n", and is NUL-terminated.
 *
 * box_lines_format_event() formats, but doesn't write, the line
 *   EVT,<seq>,<OUTCOME>*HH
 * for the sequence number and outcome it is given.
 */
#ifndef BOX_LINES_H
#define BOX_LINES_H

#include <stdbool.h>
#include <stdint.h>

#include "box_core.h"

/* Room for the longest line, "EVT,4294967295,OPENED_OCCUPIED*HH\n" (34),
 * and its NUL. */
#define BOX_LINE_MAX 40

/* The line that opens a session after every boot (product.md section 5),
 * and the sequence number its ACK carries: `ACK,0`. Events are numbered
 * from 1, so no Event's ACK can be taken for it. The Event sender
 * (box_sender.h) sends it. 0xB6 is the CRC-8 of "BOOT" (box_checksum.h). */
#define BOX_BOOT_LINE "BOOT*B6\n"
#define BOX_BOOT_SEQ  0u

/* How long after the last STS line the Box status is sent again. Well under
 * the Dashboard's ~30 s offline threshold. */
#define BOX_STS_HEARTBEAT_MS 5000u

typedef struct {
    void *context;
    void (*write_line)(void *context, const char *line);
} BoxLinesOutput;

/* Declared here so it can live in a static variable; treat the fields as
 * private. */
typedef struct {
    BoxLinesOutput output;
    bool status_known;      /* false until the first Box status change */
    BoxStatus status;       /* the last one, repeated by the heartbeat */
    uint32_t last_sts_at_ms;
} BoxLines;

void box_lines_init(BoxLines *lines, BoxLinesOutput output);

/* The BoxCoreOutputs that route the core's Box status changes to `lines`.
 * Every other callback (on_event too) is NULL. */
BoxCoreOutputs box_lines_core_outputs(BoxLines *lines);

/* Write one STS line now and restart the heartbeat wait. */
void box_lines_on_box_status_changed(BoxLines *lines, BoxStatus status, uint32_t now_ms);

/* Put "EVT,<seq>,<OUTCOME>*HH\n" and its NUL in `line`. */
void box_lines_format_event(char line[BOX_LINE_MAX], uint32_t seq, BoxEvent event);

/* Write the heartbeat STS line when it is due. `now_ms` may wrap around. */
void box_lines_tick(BoxLines *lines, uint32_t now_ms);

#endif /* BOX_LINES_H */
