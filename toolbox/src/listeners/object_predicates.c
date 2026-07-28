#include "object_predicates.h"

/*
 * Listener state matrix:
 *
 *   state       | start | stop | restart
 *   ------------+-------+------+--------
 *   CONFIGURED  | yes   | no   | no
 *   STARTING    | no    | no   | no
 *   RUNNING     | no    | yes  | yes
 *   STOPPING    | no    | no   | no
 *   STOPPED     | yes   | no   | yes
 *   ERROR       | yes   | no   | yes
 *
 * Stop is RUNNING-only, matching the doc's literal
 * STARTING->RUNNING->STOPPING->STOPPED/ERROR transition list - canceling
 * a listener still mid-bind/listen is deliberately out of scope for now.
 * Restart is available anywhere a listener has something to restart
 * from (it has run before, or failed to) but not from CONFIGURED (never
 * started - use Start) or the two transitional states.
 */

static bool listener_can_start(ListenerState state) {
    return state == LISTENER_STATE_CONFIGURED || state == LISTENER_STATE_STOPPED || state == LISTENER_STATE_ERROR;
}

static bool listener_can_stop(ListenerState state) {
    return state == LISTENER_STATE_RUNNING;
}

static bool listener_can_restart(ListenerState state) {
    return state == LISTENER_STATE_RUNNING || state == LISTENER_STATE_STOPPED || state == LISTENER_STATE_ERROR;
}

/*
 * Connection state matrix:
 *
 *   state        | stop (disconnect) | open_terminal | wait_for_reconnection
 *   -------------+--------------------+---------------+-----------------------
 *   CONNECTED    | yes                | yes            | no
 *   DISCONNECTED | no                 | yes            | yes
 *
 * open_terminal is "no network op" per the doc, so it's available in
 * both states - a disconnected connection opens read-only against its
 * retained history. wait_for_reconnection only makes sense once a
 * target has actually disconnected.
 */

static bool connection_can_stop(ConnectionState state) {
    return state == CONNECTION_STATE_CONNECTED;
}

static bool connection_can_open_terminal(ConnectionState state) {
    (void)state;
    return true;
}

static bool connection_can_wait_for_reconnection(ConnectionState state) {
    return state == CONNECTION_STATE_DISCONNECTED;
}

bool object_can_start(const ManagedObject *obj) {
    if (obj->type != MANAGED_OBJECT_LISTENER) {
        return false; /* Connections never "start" - they arrive via accept or reconnection. */
    }
    return listener_can_start(obj->listener.runtime.state);
}

bool object_can_stop(const ManagedObject *obj) {
    if (obj->type == MANAGED_OBJECT_LISTENER) {
        return listener_can_stop(obj->listener.runtime.state);
    }
    return connection_can_stop(obj->connection.state);
}

bool object_can_restart(const ManagedObject *obj) {
    if (obj->type != MANAGED_OBJECT_LISTENER) {
        return false; /* Connections reconnect, they don't restart. */
    }
    return listener_can_restart(obj->listener.runtime.state);
}

bool object_can_open_terminal(const ManagedObject *obj) {
    if (obj->type != MANAGED_OBJECT_CONNECTION) {
        return false; /* Terminal views attach to connections, not listeners. */
    }
    return connection_can_open_terminal(obj->connection.state);
}

bool object_can_wait_for_reconnection(const ManagedObject *obj) {
    if (obj->type != MANAGED_OBJECT_CONNECTION) {
        return false;
    }
    return connection_can_wait_for_reconnection(obj->connection.state);
}
