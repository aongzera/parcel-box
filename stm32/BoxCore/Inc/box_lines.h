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
 *   2. Pass box_lines_core_outputs() to box_core_init(), so every Event and
 *      Box status change becomes a line.
 *   3. Call box_lines_tick() as often as box_core_tick() (e.g. every 10 ms)
 *      for the heartbeat.
 *
 * Lines written:
 *   STS,<EMPTY|OCCUPIED>,<LID_OPEN|LID_CLOSED>  on every Box status change,
 *       and again every BOX_STS_HEARTBEAT_MS after the last STS. No STS at
 *       all, not even a heartbeat, until the first Box status change.
 *   EVT,<seq>,<OUTCOME>  once per Event. seq starts at 1 at power-up and
 *       goes up by one per Event. Sent once: no ACK or resend yet (#27+).
 * Each line passed to write_line ends in "\n" and is NUL-terminated.
 */
#ifndef BOX_LINES_H
#define BOX_LINES_H

#include <stdbool.h>
#include <stdint.h>

#include "box_core.h"

/* Room for the longest line, "EVT,4294967295,OPENED_OCCUPIED\n", and its NUL. */
#define BOX_LINE_MAX 40

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
    uint32_t next_seq;
} BoxLines;

void box_lines_init(BoxLines *lines, BoxLinesOutput output);

/* The BoxCoreOutputs that route the core's Events and Box status changes to
 * `lines`. Every other callback is NULL. */
BoxCoreOutputs box_lines_core_outputs(BoxLines *lines);

/* Write one STS line now and restart the heartbeat wait. */
void box_lines_on_box_status_changed(BoxLines *lines, BoxStatus status, uint32_t now_ms);

/* Write one EVT line with the next sequence number. */
void box_lines_on_event(BoxLines *lines, BoxEvent event, uint32_t now_ms);

/* Write the heartbeat STS line when it is due. `now_ms` may wrap around. */
void box_lines_tick(BoxLines *lines, uint32_t now_ms);

#endif /* BOX_LINES_H */
