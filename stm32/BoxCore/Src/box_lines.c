/* UART line writer: see box_lines.h. */
#include "box_lines.h"

#include <string.h>

void box_lines_init(BoxLines *lines, BoxLinesOutput output)
{
    memset(lines, 0, sizeof *lines);
    lines->output = output;
    lines->next_seq = 1;
}

static void write_line(BoxLines *lines, const char *line)
{
    if (lines->output.write_line != NULL) {
        lines->output.write_line(lines->output.context, line);
    }
}

static const char *event_name(BoxEvent event)
{
    switch (event) {
    case BOX_EVENT_DELIVERED:       return "DELIVERED";
    case BOX_EVENT_COLLECTED:       return "COLLECTED";
    case BOX_EVENT_OPENED_OCCUPIED: return "OPENED_OCCUPIED";
    case BOX_EVENT_OPENED_EMPTY:    return "OPENED_EMPTY";
    }
    return "UNKNOWN";
}

/* Append `value` in decimal. No printf, so the firmware stays small. */
static void append_uint(char *line, uint32_t value)
{
    char digits[10];
    int n = 0;
    size_t len = strlen(line);
    do {
        digits[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (n > 0) {
        line[len++] = digits[--n];
    }
    line[len] = '\0';
}

static void write_status(BoxLines *lines, uint32_t now_ms)
{
    char line[BOX_LINE_MAX];
    strcpy(line, "STS,");
    strcat(line, lines->status.occupied ? "OCCUPIED" : "EMPTY");
    strcat(line, lines->status.lid_open ? ",LID_OPEN\n" : ",LID_CLOSED\n");
    lines->last_sts_at_ms = now_ms;
    write_line(lines, line);
}

void box_lines_on_box_status_changed(BoxLines *lines, BoxStatus status, uint32_t now_ms)
{
    lines->status = status;
    lines->status_known = true;
    write_status(lines, now_ms);
}

void box_lines_on_event(BoxLines *lines, BoxEvent event, uint32_t now_ms)
{
    char line[BOX_LINE_MAX];
    (void)now_ms;
    strcpy(line, "EVT,");
    append_uint(line, lines->next_seq++);
    strcat(line, ",");
    strcat(line, event_name(event));
    strcat(line, "\n");
    write_line(lines, line);
}

void box_lines_tick(BoxLines *lines, uint32_t now_ms)
{
    /* Unsigned subtraction keeps this right when now_ms wraps around. */
    if (lines->status_known &&
        (uint32_t)(now_ms - lines->last_sts_at_ms) >= BOX_STS_HEARTBEAT_MS) {
        write_status(lines, now_ms);
    }
}

static void core_event(void *context, BoxEvent event, uint32_t now_ms)
{
    box_lines_on_event(context, event, now_ms);
}

static void core_box_status_changed(void *context, BoxStatus status, uint32_t now_ms)
{
    box_lines_on_box_status_changed(context, status, now_ms);
}

BoxCoreOutputs box_lines_core_outputs(BoxLines *lines)
{
    BoxCoreOutputs outputs;
    outputs.context = lines;
    outputs.on_event = core_event;
    outputs.on_box_status_changed = core_box_status_changed;
    return outputs;
}
