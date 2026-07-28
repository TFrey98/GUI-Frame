#include "listener_system.h"

#include <stdlib.h>

ListenerSystem *listener_system_create(void) {
    ListenerSystem *system = malloc(sizeof(ListenerSystem));
    system->registry = object_registry_create();
    system->events = event_queue_create();
    system->listener_manager = listener_manager_create(system->registry, system->events);
    system->connection_manager = connection_manager_create(system->registry, system->events);
    return system;
}

void listener_system_destroy(ListenerSystem *system) {
    if (!system) {
        return;
    }
    listener_manager_destroy(system->listener_manager);
    connection_manager_destroy(system->connection_manager);
    event_queue_destroy(system->events);
    object_registry_destroy(system->registry);
    free(system);
}

int listener_system_pump(ListenerSystem *system, ListenerEvent *out, int max_out) {
    /* Always dispatch from our own buffer, regardless of what the
     * caller passed - dispatch correctness must not depend on the
     * caller wanting a copy of the events too (out may be NULL). */
    ListenerEvent local[32];
    int n = listener_manager_process_events(system->listener_manager, local, 32);
    for (int i = 0; i < n; i++) {
        if (local[i].type == LISTENER_EVENT_CONNECTION_OPENED) {
            connection_manager_start_worker(system->connection_manager, local[i].sequence);
        } else if (local[i].type == LISTENER_EVENT_CONNECTION_CLOSED) {
            connection_manager_handle_closed(system->connection_manager, local[i].object_id);
        }
        if (out && i < max_out) {
            out[i] = local[i];
        }
    }
    connection_manager_drain_data(system->connection_manager);
    return n;
}
