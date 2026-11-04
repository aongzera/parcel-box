#include "box_core.h"

void box_core_init(BoxCore *core, BoxCoreOutputs outputs,
                   uint32_t now_ms, BoxReadings readings)
{
    core->outputs = outputs;
    core->occupied = readings.parcel_present;
    core->lid_open = readings.lid_open;
    core->raw_lid_open = readings.lid_open;
    core->raw_lid_changed_at_ms = now_ms;
    core->settling = false;
    core->lid_closed_at_ms = 0;
}

static void emit_event(BoxCore *core, BoxEvent event, uint32_t now_ms)
{
    if (core->outputs.on_event) {
        core->outputs.on_event(core->outputs.context, event, now_ms);
    }
}

/* The outcome of a Lid cycle, from the state before it and after the settle
 * (product.md section 4). */
static BoxEvent lid_cycle_outcome(bool was_occupied, bool is_occupied)
{
    if (was_occupied) {
        return is_occupied ? BOX_EVENT_OPENED_OCCUPIED : BOX_EVENT_COLLECTED;
    }
    return is_occupied ? BOX_EVENT_DELIVERED : BOX_EVENT_OPENED_EMPTY;
}

/* The settle is over: emit exactly one outcome for this Lid cycle. */
static void finish_lid_cycle(BoxCore *core, uint32_t now_ms, bool parcel_present)
{
    bool was_occupied = core->occupied;
    core->occupied = parcel_present;
    emit_event(core, lid_cycle_outcome(was_occupied, core->occupied), now_ms);
}

/* Parcel presence is read only when the settle ends, never while the Lid is
 * open, so a hand inside doesn't count. If the Lid reopens during the
 * settle, the Lid cycle isn't over: the settle restarts on the next close,
 * and `occupied` still holds the state from before the first open. */
void box_core_tick(BoxCore *core, uint32_t now_ms, BoxReadings readings)
{
    /* Debounce: a raw Lid change counts only once it has held for
     * BOX_LID_DEBOUNCE_MS. Unsigned subtraction keeps the time checks right
     * when now_ms wraps around. */
    if (readings.lid_open != core->raw_lid_open) {
        core->raw_lid_open = readings.lid_open;
        core->raw_lid_changed_at_ms = now_ms;
    }
    if (core->raw_lid_open != core->lid_open &&
        (uint32_t)(now_ms - core->raw_lid_changed_at_ms) >= BOX_LID_DEBOUNCE_MS) {
        if (core->lid_open) {
            /* The Lid has closed. Time the settle from the real close. */
            core->settling = true;
            core->lid_closed_at_ms = core->raw_lid_changed_at_ms;
        }
        core->lid_open = core->raw_lid_open;
    }

    /* Finish the settle only while the raw reading has been closed for a
     * full debounce, the same test that makes a close count. Anything less
     * may be a reopen, bouncing or not, with a hand in the beam; if it was
     * only a glitch, the settle finishes one debounce after it. In a normal
     * settle the Lid has long been stable, so the timing is unchanged. */
    if (core->settling && !core->lid_open && !core->raw_lid_open &&
        (uint32_t)(now_ms - core->raw_lid_changed_at_ms) >= BOX_LID_DEBOUNCE_MS &&
        (uint32_t)(now_ms - core->lid_closed_at_ms) >= BOX_SETTLE_TIME_MS) {
        core->settling = false;
        finish_lid_cycle(core, now_ms, readings.parcel_present);
    }
}
