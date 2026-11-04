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
 *   3. Call box_core_tick() periodically (e.g. every 10 ms) with the current
 *      time and the raw, undebounced sensor readings.
 * The core calls your callbacks from inside box_core_tick().
 */
#ifndef BOX_CORE_H
#define BOX_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* Time to wait after the Lid closes before sampling Parcel presence, so a
 * hand still inside or a parcel still settling doesn't count. */
#define BOX_SETTLE_TIME_MS 2000u

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
 * to these meanings; the core does the debouncing and the settle wait. */
typedef struct {
    bool lid_open;
    bool parcel_present;
} BoxReadings;

/* Where the core sends what it decides. Each callback gets `context` back. */
typedef struct {
    void *context;
    void (*on_event)(void *context, BoxEvent event, uint32_t now_ms);
    void (*on_box_status_changed)(void *context, BoxStatus status, uint32_t now_ms);
} BoxCoreOutputs;

/* The core's state. Declared here so it can live in a static variable with
 * no heap; treat the fields as private. */
typedef struct {
    BoxCoreOutputs outputs;
    bool occupied;
    bool lid_open;
    bool settling;
    uint32_t lid_closed_at_ms;
} BoxCore;

/* Power-up: take the first readings as the starting Box state. */
void box_core_init(BoxCore *core, BoxCoreOutputs outputs,
                   uint32_t now_ms, BoxReadings readings);

/* Feed the current time and raw readings. `now_ms` may wrap around. */
void box_core_tick(BoxCore *core, uint32_t now_ms, BoxReadings readings);

#endif /* BOX_CORE_H */
