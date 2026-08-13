#ifndef WORKBENCH_CONNECTION_H
#define WORKBENCH_CONNECTION_H

#include <stdbool.h>
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
    void *tls;     /* NULL for a plain (reverse-TCP/HTTP) connection; an SSL* for an HTTPS
                     * one, opaque here so this header never needs to know OpenSSL exists.
                     * A pass-through only - connection_worker.c (the sole owner while a
                     * worker thread is doing I/O with it) frees it, never this registry;
                     * see connection_worker.c's own comment for why. */
    TerminalHistory *history; /* everything received; retained even after DISCONNECTED */

    /* True from the moment the writer view submits a line (Enter) until
     * it starts typing the next one - the "command sent, awaiting
     * response" window. Received bytes are only persisted to the
     * database (still always fed into `history` for display/replay)
     * while this is true, so a fast typist's own echoed keystrokes -
     * indistinguishable from real output at the byte level - never reach
     * the database no matter how many arrive in one drain. See
     * ui_gtk_connection_terminal.c's commit handler for where this
     * flips. */
    bool capturing_output;
} Connection;

#endif /* WORKBENCH_CONNECTION_H */
