#ifndef WORKBENCH_EVENT_QUEUE_H
#define WORKBENCH_EVENT_QUEUE_H

#include "listener_event.h"

typedef struct EventQueue EventQueue;

EventQueue *event_queue_create(void);
/* Destroy only after producers have stopped; pending value events are
 * discarded, and the queue does not own resources referenced by payloads. */
void event_queue_destroy(EventQueue *queue);

/* Thread-safe from any thread - workers push events as network state
 * changes; never blocks. */
void event_queue_push(EventQueue *queue, ListenerEvent event);

/* Thread-safe; intended for the GUI thread to drain each frame/tick.
 * Never blocks. Returns 0 and leaves *out untouched if the queue was
 * empty, 1 if an event was popped into *out. */
int event_queue_try_pop(EventQueue *queue, ListenerEvent *out);

#endif /* WORKBENCH_EVENT_QUEUE_H */
