/*
 * Checkpoint: headless (no GUI) reverse-TCP harness.
 *   - "nc connects -> event fires": a real client connect() stands in
 *     for nc; the resulting CONNECTION_OPENED event and registered
 *     Connection are verified.
 *   - "port-in-use -> clean START_FAILED": two listeners on the same
 *     address/port - the second deterministically fails once the first
 *     is confirmed bound.
 *   - "stop unblocks accept without a leak": every stop() below is
 *     required to produce STOPPED within a short bound, proving the
 *     self-pipe actually wakes poll() rather than accept() hanging;
 *     listener_manager_process_events() joins the worker thread as part
 *     of applying that STOPPED event, so nothing is left dangling.
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

#include "listeners/listener_manager.h"
#include "listeners/object_registry.h"

static const char *TEST_NAME = "reverse_tcp_test";

static ListenerConfig make_config(const char *name, uint16_t port) {
    ListenerConfig config = {0};
    config.name = strdup(name);
    config.type = LISTENER_TYPE_REVERSE_TCP;
    config.bind_address = strdup("127.0.0.1");
    config.port = port;
    config.callback_host = strdup("203.0.113.1");
    return config;
}

/* Polls process_events (5ms between attempts, ~2s bound) until an event
 * of `type` for `id` shows up. Every drained event, not just a match,
 * is still applied to the registry as process_events always does. */
static bool wait_for_event(ListenerManager *manager, ListenerEventType type, uint64_t id, ListenerEvent *out) {
    const int timeout_ms = 2000;
    const int step_ms = 5;
    int waited = 0;
    while (waited < timeout_ms) {
        ListenerEvent events[16];
        int n = listener_manager_process_events(manager, events, 16);
        for (int i = 0; i < n; i++) {
            if (events[i].type == type && events[i].object_id == id) {
                if (out) {
                    *out = events[i];
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

static int test_connect_and_stop(ObjectRegistry *registry, ListenerManager *manager) {
    int status = 0;
    uint64_t id = listener_manager_create_listener(manager, make_config("Connect", 18080), NULL);
    if (id == 0) {
        fprintf(stderr, "%s: expected create to succeed\n", TEST_NAME);
        return 1;
    }

    if (listener_manager_start_async(manager, id) != 0) {
        fprintf(stderr, "%s: expected start_async to succeed\n", TEST_NAME);
        status = 1;
    }
    if (!wait_for_event(manager, LISTENER_EVENT_STARTED, id, NULL)) {
        fprintf(stderr, "%s: timed out waiting for STARTED\n", TEST_NAME);
        return 1; /* nothing more we can safely do without a running listener */
    }
    const Listener *listener = object_registry_get_listener(registry, id);
    if (!listener || listener->runtime.state != LISTENER_STATE_RUNNING) {
        fprintf(stderr, "%s: expected RUNNING after STARTED\n", TEST_NAME);
        status = 1;
    }

    int client_fd = connect_to("127.0.0.1", 18080);
    if (client_fd < 0) {
        fprintf(stderr, "%s: client failed to connect to a listener that reported STARTED\n", TEST_NAME);
        status = 1;
    }

    ListenerEvent opened_event;
    if (!wait_for_event(manager, LISTENER_EVENT_CONNECTION_OPENED, id, &opened_event)) {
        fprintf(stderr, "%s: timed out waiting for CONNECTION_OPENED\n", TEST_NAME);
        status = 1;
    } else if (strcmp(opened_event.remote_host, "127.0.0.1") != 0) {
        fprintf(stderr, "%s: expected remote_host '127.0.0.1', got '%s'\n", TEST_NAME, opened_event.remote_host);
        status = 1;
    }

    const Connection *connections[8];
    int conn_count = object_registry_list_connections_for_listener(registry, id, connections, 8);
    if (conn_count != 1 || connections[0]->listener_id != id ||
        strcmp(connections[0]->remote_host, "127.0.0.1") != 0) {
        fprintf(stderr, "%s: expected exactly one Connection registered under this listener\n", TEST_NAME);
        status = 1;
    }

    if (client_fd >= 0) {
        close(client_fd);
    }

    if (listener_manager_stop(manager, id) != 0) {
        fprintf(stderr, "%s: expected stop to succeed\n", TEST_NAME);
        status = 1;
    }
    if (!wait_for_event(manager, LISTENER_EVENT_STOPPED, id, NULL)) {
        fprintf(stderr, "%s: timed out waiting for STOPPED - stop() may not be unblocking accept()\n", TEST_NAME);
        status = 1;
    } else {
        listener = object_registry_get_listener(registry, id);
        if (!listener || listener->runtime.state != LISTENER_STATE_STOPPED) {
            fprintf(stderr, "%s: expected STOPPED after the STOPPED event\n", TEST_NAME);
            status = 1;
        }
    }

    return status;
}

/* Note: two of *our own* Listener configs can never collide on the same
 * bind_address:port - Phase 2's endpoint-uniqueness validation refuses
 * to create the second one in the first place, regardless of whether
 * the first is actually running. So a genuine OS-level EADDRINUSE can
 * only come from something outside the registry entirely - a plain,
 * unrelated socket standing in for some other already-running service,
 * which is also the realistic scenario an operator would actually hit. */
static int test_port_in_use(ObjectRegistry *registry, ListenerManager *manager) {
    int status = 0;
    const uint16_t port = 18081;

    int blocker_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in blocker_addr = {0};
    blocker_addr.sin_family = AF_INET;
    blocker_addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &blocker_addr.sin_addr);
    if (blocker_fd < 0 || bind(blocker_fd, (struct sockaddr *)&blocker_addr, sizeof(blocker_addr)) != 0 ||
        listen(blocker_fd, 1) != 0) {
        fprintf(stderr, "%s: failed to occupy the test port ahead of the conflict test\n", TEST_NAME);
        if (blocker_fd >= 0) {
            close(blocker_fd);
        }
        return 1;
    }

    uint64_t id = listener_manager_create_listener(manager, make_config("Blocked", port), NULL);
    if (id == 0) {
        fprintf(stderr, "%s: expected create to succeed\n", TEST_NAME);
        close(blocker_fd);
        return 1;
    }
    if (listener_manager_start_async(manager, id) != 0) {
        fprintf(stderr, "%s: expected start_async to succeed (it fails asynchronously, not here)\n", TEST_NAME);
        status = 1;
    }

    ListenerEvent failed_event;
    if (!wait_for_event(manager, LISTENER_EVENT_START_FAILED, id, &failed_event)) {
        fprintf(stderr, "%s: timed out waiting for START_FAILED\n", TEST_NAME);
        status = 1;
    } else if (failed_event.message[0] == '\0') {
        fprintf(stderr, "%s: expected START_FAILED to carry a non-empty message\n", TEST_NAME);
        status = 1;
    }
    const Listener *listener = object_registry_get_listener(registry, id);
    if (!listener || listener->runtime.state != LISTENER_STATE_ERROR) {
        fprintf(stderr, "%s: expected the listener to end up in ERROR\n", TEST_NAME);
        status = 1;
    }

    close(blocker_fd);
    return status;
}

int main(void) {
    ObjectRegistry *registry = object_registry_create();
    EventQueue *events = event_queue_create();
    ListenerManager *manager = listener_manager_create(registry, events);

    int status = 0;
    status |= test_connect_and_stop(registry, manager);
    status |= test_port_in_use(registry, manager);

    listener_manager_destroy(manager);
    event_queue_destroy(events);
    object_registry_destroy(registry);

    if (status == 0) {
        printf("%s: real accept/connect, port-conflict failure, and clean stop all verified\n", TEST_NAME);
    }
    return status;
}
