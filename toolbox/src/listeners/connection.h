#ifndef TOOLBOX_CONNECTION_H
#define TOOLBOX_CONNECTION_H

#include <stdint.h>
#include <time.h>

#include "terminal_history.h"

/*
 * Plain data only - construction and destruction belong to
 * ObjectRegistry, the sole owner of every Connection (see object_registry.h).
 */

typedef enum ConnectionState {
    CONNECTION_STATE_CONNECTED,
    CONNECTION_STATE_DISCONNECTED, /* socket closed; object + history retained */
} ConnectionState;

typedef struct Connection {
    uint64_t id;
    uint64_t listener_id; /* owning listener - resolve through the
                            * registry, never cache a pointer */
    ConnectionState state;
    char *remote_host;
    uint16_t remote_port;
    time_t connected_at;
    int socket_fd; /* the accepted connection socket; -1 if none (e.g. in tests that never opened a real one) */
    TerminalHistory *history; /* everything received; retained even after DISCONNECTED */
} Connection;

#endif /* TOOLBOX_CONNECTION_H */
