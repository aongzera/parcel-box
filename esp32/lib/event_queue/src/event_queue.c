/* The ESP32's offline Event queue: see event_queue.h. */
#include "event_queue.h"

#include <string.h>

void event_queue_init(EventQueue *queue, EventQueueOutputs outputs)
{
    memset(queue, 0, sizeof *queue);
    queue->outputs = outputs;
}

static void drop_oldest(EventQueue *queue)
{
    QueuedEvent dropped = queue->entries[queue->head];
    queue->head = (queue->head + 1u) % EVENT_QUEUE_CAPACITY;
    queue->count--;
    if (queue->outputs.on_dropped != NULL) {
        queue->outputs.on_dropped(queue->outputs.context, &dropped);
    }
}

void event_queue_push(EventQueue *queue, BridgeEvent event)
{
    QueuedEvent *entry;
    int64_t clock_s = 0;

    if (queue->outputs.clock_s != NULL) {
        clock_s = queue->outputs.clock_s(queue->outputs.context);
    }
    if (queue->count == EVENT_QUEUE_CAPACITY) {
        drop_oldest(queue);
    }
    entry = &queue->entries[(queue->head + queue->count) % EVENT_QUEUE_CAPACITY];
    entry->event = event;
    entry->time = event_time_from_clock(clock_s);   /* stamped on arrival */
    queue->count++;
}

void event_queue_tick(EventQueue *queue, uint32_t now_ms)
{
    if (queue->count == 0u || queue->outputs.upload == NULL) {
        return;
    }
    /* The wait as a signed difference, so it is right across the wrap. */
    if (queue->backing_off &&
        (int32_t)(now_ms - queue->failed_at_ms) < (int32_t)EVENT_QUEUE_RETRY_MS) {
        return;
    }
    if (!queue->outputs.upload(queue->outputs.context, &queue->entries[queue->head])) {
        queue->backing_off = true;   /* keep it at the head for next time */
        queue->failed_at_ms = now_ms;
        return;
    }
    queue->backing_off = false;
    queue->head = (queue->head + 1u) % EVENT_QUEUE_CAPACITY;
    queue->count--;
}

void event_queue_retry_now(EventQueue *queue)
{
    queue->backing_off = false;
}

uint32_t event_queue_count(const EventQueue *queue)
{
    return queue->count;
}
