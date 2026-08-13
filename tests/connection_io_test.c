/*
 * Checkpoint: "bidirectional data with a test client; disconnect from
 * either end -> DISCONNECTED but the object stays queryable."
 *
 * pump() below wires ListenerManager and ConnectionManager together the
 * way a real caller (the GUI, from a later phase) has to: it drains
 * listener_manager_process_events() (the sole queue consumer - see the
 * Phase 4 plan for why) and dispatches CONNECTION_OPENED/CONNECTION_CLOSED
 * to the two small ConnectionManager entry points that exist for
 * exactly this, then drains per-connection data. Every polling helper
 * below calls pump() instead of the raw manager calls directly, mirroring
 * the per-tick sequence a real event loop will run.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "listeners/connection_manager.h"
#include "listeners/listener_manager.h"
#include "listeners/object_registry.h"

static const char *TEST_NAME = "connection_io_test";

static ListenerConfig make_config(const char *name, uint16_t port) {
    ListenerConfig config = {0};
    config.name = strdup(name);
    config.type = LISTENER_TYPE_REVERSE_TCP;
    config.bind_address = strdup("127.0.0.1");
    config.port = port;
    config.callback_host = strdup("203.0.113.1");
    return config;
}

static int pump(ListenerManager *listeners, ConnectionManager *connections, ListenerEvent *out, int max_out) {
    /* Always dispatch from our own buffer, regardless of what the
     * caller passed (some callers pass out=NULL/max_out=0 when they
     * only care about the drain-and-dispatch side effect, e.g.
     * wait_for_history_tail below) - dispatch correctness must not
     * depend on the caller wanting a copy of the events too. */
    ListenerEvent local[32];
    int n = listener_manager_process_events(listeners, local, 32);
    for (int i = 0; i < n; i++) {
        if (local[i].type == LISTENER_EVENT_CONNECTION_OPENED) {
            connection_manager_start_worker(connections, local[i].sequence); /* sequence carries the new Connection's id */
        } else if (local[i].type == LISTENER_EVENT_CONNECTION_CLOSED) {
            connection_manager_handle_closed(connections, local[i].object_id);
        }
        if (out && i < max_out) {
            out[i] = local[i];
        }
    }
    connection_manager_drain_data(connections);
    return n;
}

static bool wait_for_event(ListenerManager *listeners, ConnectionManager *connections, ListenerEventType type,
                            uint64_t id, ListenerEvent *out_event) {
    const int timeout_ms = 2000;
    const int step_ms = 5;
    int waited = 0;
    while (waited < timeout_ms) {
        ListenerEvent events[16];
        int n = pump(listeners, connections, events, 16);
        for (int i = 0; i < n; i++) {
            if (events[i].type == type && events[i].object_id == id) {
                if (out_event) {
                    *out_event = events[i];
                }
                return true;
            }
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = step_ms * 1000 * 1000};
        nanosleep(&ts, NULL);
        waited += step_ms;
    }
    return false;
}

/* Waits until connection_id's TerminalHistory contains at least
 * expected_len bytes, then checks the last expected_len of them match
 * expected. */
static bool wait_for_history_tail(ListenerManager *listeners, ConnectionManager *connections,
                                   ObjectRegistry *registry, uint64_t connection_id, const char *expected,
                                   size_t expected_len) {
    const int timeout_ms = 2000;
    const int step_ms = 5;
    int waited = 0;
    while (waited < timeout_ms) {
        pump(listeners, connections, NULL, 0);
        const Connection *connection = object_registry_get_connection(registry, connection_id);
        if (connection) {
            size_t len = terminal_history_len(connection->history);
            if (len >= expected_len) {
                char actual[256];
                size_t n = terminal_history_read(connection->history, len - expected_len, actual, sizeof(actual));
                return n == expected_len && memcmp(actual, expected, expected_len) == 0;
            }
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = step_ms * 1000 * 1000};
        nanosleep(&ts, NULL);
        waited += step_ms;
    }
    return false;
}

static int connect_to(const char *host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Blocking-with-bound read of exactly len bytes from fd into dst.
 * Returns true iff exactly len bytes were read within the bound. */
static bool read_exact(int fd, void *dst, size_t len) {
    unsigned char *out = dst;
    size_t total = 0;
    const int timeout_ms = 2000;
    struct timeval tv = {.tv_sec = 0, .tv_usec = 50 * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int waited = 0;
    while (total < len && waited < timeout_ms) {
        ssize_t n = read(fd, out + total, len - total);
        if (n > 0) {
            total += (size_t)n;
        } else {
            waited += 50;
        }
    }
    return total == len;
}

int main(void) {
    int status = 0;

    ObjectRegistry *registry = object_registry_create();
    EventQueue *events = event_queue_create();
    ListenerManager *listeners = listener_manager_create(registry, events);
    ConnectionManager *connections = connection_manager_create(registry, events);

    uint64_t listener_id = listener_manager_create_listener(listeners, make_config("IO", 18110), NULL);
    if (listener_manager_start_async(listeners, listener_id) != 0) {
        fprintf(stderr, "%s: expected start_async to succeed\n", TEST_NAME);
        status = 1;
    }
    if (!wait_for_event(listeners, connections, LISTENER_EVENT_STARTED, listener_id, NULL)) {
        fprintf(stderr, "%s: timed out waiting for listener STARTED\n", TEST_NAME);
        return 1; /* nothing more we can safely do without a running listener */
    }

    /* --- Bidirectional data --- */
    int client1_fd = connect_to("127.0.0.1", 18110);
    if (client1_fd < 0) {
        fprintf(stderr, "%s: client1 failed to connect\n", TEST_NAME);
        status = 1;
    }

    ListenerEvent opened_event;
    if (!wait_for_event(listeners, connections, LISTENER_EVENT_CONNECTION_OPENED, listener_id, &opened_event)) {
        fprintf(stderr, "%s: timed out waiting for CONNECTION_OPENED\n", TEST_NAME);
        return 1;
    }
    uint64_t connection1_id = opened_event.sequence;

    const char *from_client = "hello from client";
    if (write(client1_fd, from_client, strlen(from_client)) != (ssize_t)strlen(from_client)) {
        fprintf(stderr, "%s: client1 write failed\n", TEST_NAME);
        status = 1;
    }
    if (!wait_for_history_tail(listeners, connections, registry, connection1_id, from_client, strlen(from_client))) {
        fprintf(stderr, "%s: connection history never received the client's bytes\n", TEST_NAME);
        status = 1;
    }

    const char *from_server = "hello from server";
    if (connection_manager_send(connections, connection1_id, from_server, strlen(from_server)) != 0) {
        fprintf(stderr, "%s: connection_manager_send failed\n", TEST_NAME);
        status = 1;
    }
    char received[64] = {0};
    if (!read_exact(client1_fd, received, strlen(from_server)) ||
        memcmp(received, from_server, strlen(from_server)) != 0) {
        fprintf(stderr, "%s: client1 did not receive the server's bytes intact\n", TEST_NAME);
        status = 1;
    }

    /* --- Disconnect from the local end --- */
    if (connection_manager_disconnect(connections, connection1_id) != 0) {
        fprintf(stderr, "%s: expected disconnect to succeed on a CONNECTED connection\n", TEST_NAME);
        status = 1;
    }
    if (!wait_for_event(listeners, connections, LISTENER_EVENT_CONNECTION_CLOSED, connection1_id, NULL)) {
        fprintf(stderr, "%s: timed out waiting for CONNECTION_CLOSED after local disconnect\n", TEST_NAME);
        status = 1;
    }
    {
        const Connection *connection = object_registry_get_connection(registry, connection1_id);
        if (!connection || connection->state != CONNECTION_STATE_DISCONNECTED) {
            fprintf(stderr, "%s: expected connection1 to be DISCONNECTED but still queryable\n", TEST_NAME);
            status = 1;
        }
    }
    close(client1_fd);

    /* --- Disconnect from the remote end --- */
    int client2_fd = connect_to("127.0.0.1", 18110);
    if (client2_fd < 0) {
        fprintf(stderr, "%s: client2 failed to connect\n", TEST_NAME);
        status = 1;
    }
    ListenerEvent opened_event2;
    if (!wait_for_event(listeners, connections, LISTENER_EVENT_CONNECTION_OPENED, listener_id, &opened_event2)) {
        fprintf(stderr, "%s: timed out waiting for CONNECTION_OPENED for client2\n", TEST_NAME);
        return 1;
    }
    uint64_t connection2_id = opened_event2.sequence;

    /* No connection_manager_disconnect() call here - just hang up the
     * raw client socket, simulating the remote end closing on its own. */
    close(client2_fd);

    if (!wait_for_event(listeners, connections, LISTENER_EVENT_CONNECTION_CLOSED, connection2_id, NULL)) {
        fprintf(stderr, "%s: timed out waiting for CONNECTION_CLOSED after remote hangup\n", TEST_NAME);
        status = 1;
    }
    {
        const Connection *connection = object_registry_get_connection(registry, connection2_id);
        if (!connection || connection->state != CONNECTION_STATE_DISCONNECTED) {
            fprintf(stderr, "%s: expected connection2 to be DISCONNECTED but still queryable\n", TEST_NAME);
            status = 1;
        }
    }

    if (listener_manager_stop(listeners, listener_id) != 0) {
        fprintf(stderr, "%s: expected listener stop to succeed\n", TEST_NAME);
        status = 1;
    }
    wait_for_event(listeners, connections, LISTENER_EVENT_STOPPED, listener_id, NULL);

    connection_manager_destroy(connections);
    listener_manager_destroy(listeners);
    event_queue_destroy(events);
    object_registry_destroy(registry);

    if (status == 0) {
        printf("%s: bidirectional data and disconnect-from-either-end both verified\n", TEST_NAME);
    }
    return status;
}
