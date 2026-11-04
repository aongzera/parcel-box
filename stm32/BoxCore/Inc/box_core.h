/*
 * Box logic core: decides what each Lid cycle means.
 *
 * Pure C99 with no HAL or hardware includes, so it runs both in the STM32
 * firmware and in the PC test suite (`make test`). See product.md section 4
 * and ADR 0001.
 *
 * How to use it:
 *   1. Fill a BoxCoreOutputs with your callbacks (any may be NULL).
 *   2. Call box_core_init() once at power-up with the first sensor readings.
 *      Only their Lid reading is used; parcel_present is ignored until the
 *      power-up settle ends (see "Power-up" below).
 *   3. Call box_core_tick() periodically (e.g. every 10 ms) with the current
 *      time and the raw, undebounced sensor readings.
 * The core calls your callbacks from inside box_core_tick() only, never from
 * box_core_init().
 *
 * Power-up: the core reads its starting Empty/Occupied state once the Lid
 * has been closed for one settle (BOX_SETTLE_TIME_MS after boot, or after
 * the Lid closes if it was open at boot), and reports it as the first Box
 * status, with no Event. A Lid cycle that starts before then is part of
 * power-up and emits no Event either.
 *
 * Send no STS line, not even a heartbeat, until the first
 * on_box_status_changed. With the Lid open at boot, that is only after it
 * closes and settles. Send the boot STS line from that first callback.
 */
#ifndef BOX_CORE_H
#define BOX_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* Time to wait after the Lid closes before sampling Parcel presence, so a
 * hand still inside or a parcel still settling doesn't count. */
#define BOX_SETTLE_TIME_MS 2000u

/* How long the raw Lid reading must stay the same before the core accepts
 * it as a Lid open or close, so reed switch bounce or a wobble is ignored.
 * The settle is still timed from when the Lid actually closed. */
#define BOX_LID_DEBOUNCE_MS 50u

/* The outcome of one Lid cycle. Names match the UART protocol. */
typedef enum {
    BOX_EVENT_DELIVERED,
    BOX_EVENT_COLLECTED,
    BOX_EVENT_OPENED_OCCUPIED,
    BOX_EVENT_OPENED_EMPTY
} BoxEvent;

/* A snapshot of the Box right now. */
typedef struct {
    bool occupied;
    bool lid_open;
} BoxStatus;

/* Raw sensor readings for one tick. The hardware adapter converts pin levels
 * to these meanings and does no filtering. The core debounces `lid_open`,
 * and reads `parcel_present` only once the settle after a close ends. */
typedef struct {
    bool lid_open;
    bool parcel_present;
} BoxReadings;

/* Where the core sends what it decides. Each callback gets `context` back.
 *   on_event: once per finished Lid cycle (never for the power-up one).
 *   on_box_status_changed: the full Box status, first when the starting
 *     state is known, then on every debounced Lid open or close and every
 *     Empty/Occupied change, and never when nothing changed. Keep the last
 *     one for the STS heartbeat, but send no STS (not even a heartbeat)
 *     until this has been called once: before then there is no Box status
 *     to send, and a zero-filled one would be a false EMPTY,LID_CLOSED. */
typedef struct {
    void *context;
    void (*on_event)(void *context, BoxEvent event, uint32_t now_ms);
    void (*on_box_status_changed)(void *context, BoxStatus status, uint32_t now_ms);
} BoxCoreOutputs;

/* The core's state. Declared here so it can live in a static variable with
 * no heap; treat the fields as private. */
typedef struct {
    BoxCoreOutputs outputs;
    bool starting_state_known;  /* false until the power-up settle ends */
    bool occupied;
    bool lid_open;              /* debounced */
    bool raw_lid_open;          /* last raw reading */
    uint32_t raw_lid_changed_at_ms;
    bool settling;
    uint32_t lid_closed_at_ms;
} BoxCore;

/* Power-up. Emits nothing: see "Power-up" at the top of this file. */
void box_core_init(BoxCore *core, BoxCoreOutputs outputs,
                   uint32_t now_ms, BoxReadings readings);

/* Feed the current time and raw readings. `now_ms` may wrap around. */
void box_core_tick(BoxCore *core, uint32_t now_ms, BoxReadings readings);

#endif /* BOX_CORE_H */
