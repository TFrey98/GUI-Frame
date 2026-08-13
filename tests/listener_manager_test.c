/*
 * Checkpoint: "the STARTING->RUNNING->STOPPING->STOPPED->ERROR
 * transitions + events are locked down."
 *
 * start_async/stop now spawn/signal a real worker thread (Phase 3), so
 * this drives the state machine through the real async path - polling
 * listener_manager_process_events() for outcomes rather than asserting
 * synchronously right after each call, matching how any real caller
 * (this project's own reverse_tcp_test.c, and eventually the GUI) has
 * to observe them. A separate section below tests
 * report_start_result/report_stop_result's own apply-logic in true
 * isolation, by forcing a listener into STARTING/STOPPING directly via
 * object_registry_get_listener_runtime_mut() instead of going through
 * start_async/stop - deliberate white-box use, same as Phase 1's
 * object_predicates_test.c constructing states nothing else can reach.
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

#include "listeners/event_queue.h"
#include "listeners/listener_manager.h"
#include "listeners/object_registry.h"

static const char *TEST_NAME = "listener_manager_test";

static ListenerConfig make_config(const char *name, uint16_t port) {
    ListenerConfig config = {0};
    config.name = strdup(name);
    config.type = LISTENER_TYPE_REVERSE_TCP;
    config.bind_address = strdup("127.0.0.1");
    config.port = port;
    config.callback_host = strdup("203.0.113.1");
    return config;
}

/* Polls process_events (5ms steps, ~2s bound) until an event of `type`
 * for `id` shows up, applying every drained event along the way (as
 * process_events always does) regardless of whether it matches. */
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

/* Drains and applies whatever's queued right now, with no waiting -
 * used to assert a rejected call produced no event at all. */
static bool has_pending_event(ListenerManager *manager) {
    ListenerEvent events[16];
    return listener_manager_process_events(manager, events, 16) > 0;
}

static int test_state_machine_via_real_worker(void) {
    int status = 0;
    ObjectRegistry *registry = object_registry_create();
    EventQueue *events = event_queue_create();
    ListenerManager *manager = listener_manager_create(registry, events);

    /* Happy path: CONFIGURED -> STARTING -> RUNNING -> STOPPING -> STOPPED */
    uint64_t id = listener_manager_create_listener(manager, make_config("Happy", 18090), NULL);
    if (id == 0) {
        fprintf(stderr, "%s: expected create to succeed\n", TEST_NAME);
        status = 1;
    }
    if (!wait_for_event(manager, LISTENER_EVENT_CREATED, id, NULL)) {
        fprintf(stderr, "%s: expected a CREATED event\n", TEST_NAME);
        status = 1;
    }
    if (object_registry_get_listener(registry, id)->runtime.state != LISTENER_STATE_CONFIGURED) {
        fprintf(stderr, "%s: expected CONFIGURED right after create\n", TEST_NAME);
        status = 1;
    }

    if (listener_manager_start_async(manager, id) != 0) {
        fprintf(stderr, "%s: expected start_async to succeed from CONFIGURED\n", TEST_NAME);
        status = 1;
    }
    if (!wait_for_event(manager, LISTENER_EVENT_STARTED, id, NULL)) {
        fprintf(stderr, "%s: timed out waiting for STARTED\n", TEST_NAME);
        status = 1;
    }
    {
        const Listener *listener = object_registry_get_listener(registry, id);
        if (listener->runtime.state != LISTENER_STATE_RUNNING || listener->runtime.started_at == 0) {
            fprintf(stderr, "%s: expected RUNNING with started_at set\n", TEST_NAME);
            status = 1;
        }
    }

    if (listener_manager_stop(manager, id) != 0) {
        fprintf(stderr, "%s: expected stop to succeed from RUNNING\n", TEST_NAME);
        status = 1;
    }
    if (!wait_for_event(manager, LISTENER_EVENT_STOPPED, id, NULL)) {
        fprintf(stderr, "%s: timed out waiting for STOPPED\n", TEST_NAME);
        status = 1;
    }
    if (object_registry_get_listener(registry, id)->runtime.state != LISTENER_STATE_STOPPED) {
        fprintf(stderr, "%s: expected STOPPED after the STOPPED event\n", TEST_NAME);
        status = 1;
    }

    /* Failure path: STOPPED -> STARTING -> ERROR, by binding a port a
     * plain socket already occupies (see reverse_tcp_test.c for why two
     * of our own Listener configs can never share a port). */
    int blocker_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in blocker_addr = {0};
    blocker_addr.sin_family = AF_INET;
    blocker_addr.sin_port = htons(18091);
    inet_pton(AF_INET, "127.0.0.1", &blocker_addr.sin_addr);
    if (blocker_fd < 0 || bind(blocker_fd, (struct sockaddr *)&blocker_addr, sizeof(blocker_addr)) != 0 ||
        listen(blocker_fd, 1) != 0) {
        fprintf(stderr, "%s: failed to occupy a port for the failure-path test\n", TEST_NAME);
        status = 1;
    }

    uint64_t blocked_id = listener_manager_create_listener(manager, make_config("Blocked", 18091), NULL);
    if (listener_manager_start_async(manager, blocked_id) != 0) {
        fprintf(stderr, "%s: expected start_async to succeed (fails asynchronously, not here)\n", TEST_NAME);
        status = 1;
    }
    if (!wait_for_event(manager, LISTENER_EVENT_START_FAILED, blocked_id, NULL)) {
        fprintf(stderr, "%s: timed out waiting for START_FAILED\n", TEST_NAME);
        status = 1;
    }
    {
        const Listener *listener = object_registry_get_listener(registry, blocked_id);
        if (!listener || listener->runtime.state != LISTENER_STATE_ERROR || !listener->runtime.last_error ||
            !listener->runtime.last_error[0]) {
            fprintf(stderr, "%s: expected ERROR state with a non-empty last_error\n", TEST_NAME);
            status = 1;
        }
    }
    close(blocker_fd);

    /* Restart from ERROR: the blocker is gone, so the same listener's
     * port is free now - ERROR -> STARTING -> RUNNING. */
    if (listener_manager_start_async(manager, blocked_id) != 0) {
        fprintf(stderr, "%s: expected start_async to succeed from ERROR\n", TEST_NAME);
        status = 1;
    }
    if (!wait_for_event(manager, LISTENER_EVENT_STARTED, blocked_id, NULL)) {
        fprintf(stderr, "%s: timed out waiting for STARTED after restart\n", TEST_NAME);
        status = 1;
    }
    if (object_registry_get_listener(registry, blocked_id)->runtime.state != LISTENER_STATE_RUNNING) {
        fprintf(stderr, "%s: expected RUNNING after restart\n", TEST_NAME);
        status = 1;
    }

    /* Predicate-gated rejections: neither call should change state or push an event. */
    uint64_t fresh_id = listener_manager_create_listener(manager, make_config("Fresh", 18092), NULL);
    if (!wait_for_event(manager, LISTENER_EVENT_CREATED, fresh_id, NULL)) {
        fprintf(stderr, "%s: expected a CREATED event for the fresh listener\n", TEST_NAME);
        status = 1;
    }

    if (listener_manager_stop(manager, fresh_id) != -1) {
        fprintf(stderr, "%s: expected stop() on a CONFIGURED listener to fail\n", TEST_NAME);
        status = 1;
    }
    if (has_pending_event(manager)) {
        fprintf(stderr, "%s: expected no event from stop() on a CONFIGURED listener\n", TEST_NAME);
        status = 1;
    }

    if (listener_manager_start_async(manager, blocked_id) != -1) { /* blocked_id is currently RUNNING */
        fprintf(stderr, "%s: expected start_async() on a RUNNING listener to fail\n", TEST_NAME);
        status = 1;
    }
    if (has_pending_event(manager)) {
        fprintf(stderr, "%s: expected no event from start_async() on a RUNNING listener\n", TEST_NAME);
        status = 1;
    }

    /* Invalid config: create() must reject it before touching the registry. */
    int count_before = object_registry_listener_count(registry);
    ListenerConfig bad_config = {0}; /* name left NULL -> invalid */
    bad_config.type = LISTENER_TYPE_REVERSE_TCP;
    bad_config.bind_address = strdup("127.0.0.1");
    bad_config.port = 18093;
    bad_config.callback_host = strdup("203.0.113.1");
    uint64_t bad_id = listener_manager_create_listener(manager, bad_config, NULL);
    if (bad_id != 0) {
        fprintf(stderr, "%s: expected create with an invalid config to fail\n", TEST_NAME);
        status = 1;
    }
    if (has_pending_event(manager)) {
        fprintf(stderr, "%s: expected no event from create() with an invalid config\n", TEST_NAME);
        status = 1;
    }
    if (object_registry_listener_count(registry) != count_before) {
        fprintf(stderr, "%s: expected no listener to be added for an invalid config\n", TEST_NAME);
        status = 1;
    }
    free(bad_config.bind_address);
    free(bad_config.callback_host);

    /* Clean up the listener left RUNNING before tearing down. */
    listener_manager_stop(manager, blocked_id);
    wait_for_event(manager, LISTENER_EVENT_STOPPED, blocked_id, NULL);

    listener_manager_destroy(manager);
    event_queue_destroy(events);
    object_registry_destroy(registry);
    return status;
}

/* report_start_result/report_stop_result's own apply-logic, in
 * isolation: force STARTING/STOPPING directly via
 * object_registry_get_listener_runtime_mut() instead of start_async()/
 * stop() so nothing spawns a real worker thread here. */
static int test_report_result_functions_directly(void) {
    int status = 0;
    ObjectRegistry *registry = object_registry_create();
    EventQueue *events = event_queue_create();
    ListenerManager *manager = listener_manager_create(registry, events);
    const char *step = "report_result direct";

    uint64_t id = listener_manager_create_listener(manager, make_config("Direct", 18094), NULL);
    ListenerEvent event;
    event_queue_try_pop(events, &event); /* discard CREATED */

    /* Wrong state: report_start_result while still CONFIGURED. */
    if (listener_manager_report_start_result(manager, id, true, NULL) != -1) {
        fprintf(stderr, "%s: %s: expected report_start_result to fail outside STARTING\n", TEST_NAME, step);
        status = 1;
    }

    object_registry_get_listener_runtime_mut(registry, id)->state = LISTENER_STATE_STARTING;
    if (listener_manager_report_start_result(manager, id, true, NULL) != 0) {
        fprintf(stderr, "%s: %s: expected report_start_result(true) to succeed from STARTING\n", TEST_NAME, step);
        status = 1;
    }
    if (!event_queue_try_pop(events, &event) || event.type != LISTENER_EVENT_STARTED || event.object_id != id) {
        fprintf(stderr, "%s: %s: expected a STARTED event\n", TEST_NAME, step);
        status = 1;
    }
    if (object_registry_get_listener(registry, id)->runtime.state != LISTENER_STATE_RUNNING) {
        fprintf(stderr, "%s: %s: expected RUNNING after report_start_result(true)\n", TEST_NAME, step);
        status = 1;
    }

    object_registry_get_listener_runtime_mut(registry, id)->state = LISTENER_STATE_STARTING;
    const char *error_message = "simulated failure";
    if (listener_manager_report_start_result(manager, id, false, error_message) != 0) {
        fprintf(stderr, "%s: %s: expected report_start_result(false) to succeed from STARTING\n", TEST_NAME, step);
        status = 1;
    }
    if (!event_queue_try_pop(events, &event) || event.type != LISTENER_EVENT_START_FAILED ||
        strcmp(event.message, error_message) != 0) {
        fprintf(stderr, "%s: %s: expected a START_FAILED event carrying the message\n", TEST_NAME, step);
        status = 1;
    }
    {
        const Listener *listener = object_registry_get_listener(registry, id);
        if (listener->runtime.state != LISTENER_STATE_ERROR || strcmp(listener->runtime.last_error, error_message) != 0) {
            fprintf(stderr, "%s: %s: expected ERROR with last_error set\n", TEST_NAME, step);
            status = 1;
        }
    }

    /* Wrong state: report_stop_result while in ERROR (not STOPPING). */
    if (listener_manager_report_stop_result(manager, id) != -1) {
        fprintf(stderr, "%s: %s: expected report_stop_result to fail outside STOPPING\n", TEST_NAME, step);
        status = 1;
    }

    object_registry_get_listener_runtime_mut(registry, id)->state = LISTENER_STATE_STOPPING;
    if (listener_manager_report_stop_result(manager, id) != 0) {
        fprintf(stderr, "%s: %s: expected report_stop_result to succeed from STOPPING\n", TEST_NAME, step);
        status = 1;
    }
    if (!event_queue_try_pop(events, &event) || event.type != LISTENER_EVENT_STOPPED || event.object_id != id) {
        fprintf(stderr, "%s: %s: expected a STOPPED event\n", TEST_NAME, step);
        status = 1;
    }
    if (object_registry_get_listener(registry, id)->runtime.state != LISTENER_STATE_STOPPED) {
        fprintf(stderr, "%s: %s: expected STOPPED after report_stop_result\n", TEST_NAME, step);
        status = 1;
    }

    listener_manager_destroy(manager);
    event_queue_destroy(events);
    object_registry_destroy(registry);
    return status;
}

int main(void) {
    int status = 0;
    status |= test_state_machine_via_real_worker();
    status |= test_report_result_functions_directly();

    if (status == 0) {
        printf("%s: full state machine transitions and events verified\n", TEST_NAME);
    }
    return status;
}
