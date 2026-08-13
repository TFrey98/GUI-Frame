/*
 * Table-driven check of the object_can_* predicates across every
 * ListenerState and ConnectionState - the full state matrix documented
 * in src/listeners/object_predicates.c. Builds ManagedObject values
 * directly on the stack rather than through ObjectRegistry, since no
 * manager exists yet to drive an object into most of these states.
 */
#include <stddef.h>
#include <stdio.h>

#include "listeners/object_predicates.h"

static const char *listener_state_name(ListenerState state) {
    switch (state) {
        case LISTENER_STATE_CONFIGURED: return "CONFIGURED";
        case LISTENER_STATE_STARTING: return "STARTING";
        case LISTENER_STATE_RUNNING: return "RUNNING";
        case LISTENER_STATE_STOPPING: return "STOPPING";
        case LISTENER_STATE_STOPPED: return "STOPPED";
        case LISTENER_STATE_ERROR: return "ERROR";
    }
    return "?";
}

static const char *connection_state_name(ConnectionState state) {
    switch (state) {
        case CONNECTION_STATE_CONNECTED: return "CONNECTED";
        case CONNECTION_STATE_DISCONNECTED: return "DISCONNECTED";
    }
    return "?";
}

typedef struct ListenerCase {
    ListenerState state;
    bool can_start;
    bool can_stop;
    bool can_restart;
} ListenerCase;

typedef struct ConnectionCase {
    ConnectionState state;
    bool can_stop;
    bool can_open_terminal;
    bool can_wait_for_reconnection;
} ConnectionCase;

static int check(bool actual, bool expected, const char *predicate, const char *state_name, int *status) {
    if (actual != expected) {
        fprintf(stderr, "object_predicates_test: %s(%s) = %s, expected %s\n", predicate, state_name,
                actual ? "true" : "false", expected ? "true" : "false");
        *status = 1;
    }
    return *status;
}

static int test_listener_matrix(void) {
    int status = 0;
    static const ListenerCase cases[] = {
        {LISTENER_STATE_CONFIGURED, true, false, false},
        {LISTENER_STATE_STARTING, false, false, false},
        {LISTENER_STATE_RUNNING, false, true, true},
        {LISTENER_STATE_STOPPING, false, false, false},
        {LISTENER_STATE_STOPPED, true, false, true},
        {LISTENER_STATE_ERROR, true, false, true},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ManagedObject obj = {0};
        obj.type = MANAGED_OBJECT_LISTENER;
        obj.listener.runtime.state = cases[i].state;
        const char *name = listener_state_name(cases[i].state);

        check(object_can_start(&obj), cases[i].can_start, "object_can_start(listener)", name, &status);
        check(object_can_stop(&obj), cases[i].can_stop, "object_can_stop(listener)", name, &status);
        check(object_can_restart(&obj), cases[i].can_restart, "object_can_restart(listener)", name, &status);
        check(object_can_open_terminal(&obj), false, "object_can_open_terminal(listener)", name, &status);
        check(object_can_wait_for_reconnection(&obj), false, "object_can_wait_for_reconnection(listener)", name,
              &status);
    }
    return status;
}

static int test_connection_matrix(void) {
    int status = 0;
    static const ConnectionCase cases[] = {
        {CONNECTION_STATE_CONNECTED, true, true, false},
        {CONNECTION_STATE_DISCONNECTED, false, true, true},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ManagedObject obj = {0};
        obj.type = MANAGED_OBJECT_CONNECTION;
        obj.connection.state = cases[i].state;
        const char *name = connection_state_name(cases[i].state);

        check(object_can_start(&obj), false, "object_can_start(connection)", name, &status);
        check(object_can_stop(&obj), cases[i].can_stop, "object_can_stop(connection)", name, &status);
        check(object_can_restart(&obj), false, "object_can_restart(connection)", name, &status);
        check(object_can_open_terminal(&obj), cases[i].can_open_terminal, "object_can_open_terminal(connection)",
              name, &status);
        check(object_can_wait_for_reconnection(&obj), cases[i].can_wait_for_reconnection,
              "object_can_wait_for_reconnection(connection)", name, &status);
    }
    return status;
}

int main(void) {
    int status = 0;
    status |= test_listener_matrix();
    status |= test_connection_matrix();

    if (status == 0) {
        printf("object_predicates_test: full Listener/Connection state matrix verified\n");
    }
    return status;
}
