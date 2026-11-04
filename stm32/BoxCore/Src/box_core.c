#include "box_core.h"

void box_core_init(BoxCore *core, BoxCoreOutputs outputs,
                   uint32_t now_ms, BoxReadings readings)
{
    (void)now_ms;
    core->outputs = outputs;
    core->occupied = readings.parcel_present;
    core->lid_open = readings.lid_open;
    core->settling = false;
    core->lid_closed_at_ms = 0;
}

static void emit_event(BoxCore *core, BoxEvent event, uint32_t now_ms)
{
    if (core->outputs.on_event) {
        core->outputs.on_event(core->outputs.context, event, now_ms);
    }
}

/* The settle is over: compare the state before the Lid cycle with now. */
static void finish_lid_cycle(BoxCore *core, uint32_t now_ms, bool parcel_present)
{
    bool was_occupied = core->occupied;
    core->occupied = parcel_present;
    if (!was_occupied && core->occupied) {
        emit_event(core, BOX_EVENT_DELIVERED, now_ms);
    }
}

void box_core_tick(BoxCore *core, uint32_t now_ms, BoxReadings readings)
{
    if (core->lid_open && !readings.lid_open) {
        core->settling = true;
        core->lid_closed_at_ms = now_ms;
    }
    core->lid_open = readings.lid_open;

    /* Unsigned subtraction keeps this right when now_ms wraps around. */
    if (core->settling && !core->lid_open &&
        (uint32_t)(now_ms - core->lid_closed_at_ms) >= BOX_SETTLE_TIME_MS) {
        core->settling = false;
        finish_lid_cycle(core, now_ms, readings.parcel_present);
    }
}
