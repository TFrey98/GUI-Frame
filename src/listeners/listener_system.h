#ifndef WORKBENCH_LISTENER_SYSTEM_H
#define WORKBENCH_LISTENER_SYSTEM_H

#include "connection_manager.h"
#include "event_queue.h"
#include "listener_manager.h"
#include "object_registry.h"

/*
 * Bundles the registry and both managers so App can own one thing and
 * pass it down to the GUI layer, instead of threading four separate
 * pointers through everything - just a bag of already
 * independently-encapsulated pointers, same treatment as Connection/
 * ListenerConfig, not opaque, since the GUI layer needs direct field
 * access to query the registry and post intents to the managers.
 */
typedef struct ListenerSystem {
    ObjectRegistry *registry;
    EventQueue *events;
    ListenerManager *listener_manager;
    ConnectionManager *connection_manager;
} ListenerSystem;

ListenerSystem *listener_system_create(void);
void listener_system_destroy(ListenerSystem *system);

/* Drains listener_manager_process_events(), dispatches
 * LISTENER_EVENT_CONNECTION_OPENED to connection_manager_start_worker()
 * and LISTENER_EVENT_CONNECTION_CLOSED to
 * connection_manager_handle_closed() (the same wiring used by headless
 * tests' pump helpers), then calls
 * connection_manager_drain_data(). Call once per GUI tick.
 *
 * Same out/max_out contract as listener_manager_process_events(): every
 * drained event (already-dispatched CONNECTION_OPENED/CLOSED included)
 * is also copied into out, so the caller can react to other event types
 * too (e.g. opening a tab on LISTENER_EVENT_STARTED) without a second,
 * competing consumer of the queue. If out is NULL, drains the whole
 * queue without copying (max_out is ignored). Returns the number of
 * events processed. */
int listener_system_pump(ListenerSystem *system, ListenerEvent *out, int max_out);

#endif /* WORKBENCH_LISTENER_SYSTEM_H */
