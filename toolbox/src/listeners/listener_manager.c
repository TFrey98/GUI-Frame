#include "listener_manager.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "object_predicates.h"
#include "tcp_worker.h"

static void add_error(ListenerConfigValidation *out, ListenerConfigField field, const char *message) {
    if (out->error_count >= LISTENER_CONFIG_MAX_ERRORS) {
        return;
    }
    out->errors[out->error_count].field = field;
    snprintf(out->errors[out->error_count].message, sizeof(out->errors[out->error_count].message), "%s", message);
    out->error_count++;
}

bool listener_config_validate(const ObjectRegistry *registry, const ListenerConfig *config,
                               ListenerConfigValidation *out) {
    out->error_count = 0;

    if (!config->name || !*config->name) {
        add_error(out, LISTENER_CONFIG_FIELD_NAME, "Name is required");
    }

    /* Only reverse TCP is implemented so far; HTTP/HTTPS are rejected
     * here until the phases that add those providers. */
    if (config->type != LISTENER_TYPE_REVERSE_TCP) {
        add_error(out, LISTENER_CONFIG_FIELD_TYPE, "Unsupported listener type");
    }

    if (config->port == 0) {
        add_error(out, LISTENER_CONFIG_FIELD_PORT, "Port must be between 1 and 65535");
    }

    {
        struct in_addr addr4;
        struct in6_addr addr6;
        bool bind_ok = config->bind_address && (inet_pton(AF_INET, config->bind_address, &addr4) == 1 ||
                                                 inet_pton(AF_INET6, config->bind_address, &addr6) == 1);
        if (!bind_ok) {
            add_error(out, LISTENER_CONFIG_FIELD_BIND_ADDRESS, "Bind address must be a valid IPv4 or IPv6 address");
        }
    }

    if (!config->callback_host || !*config->callback_host) {
        add_error(out, LISTENER_CONFIG_FIELD_CALLBACK_HOST, "Callback host is required");
    } else if (strcmp(config->callback_host, "0.0.0.0") == 0 || strcmp(config->callback_host, "::") == 0) {
        /* Not a general hostname/IP syntax check - callback_host may
         * legitimately be a domain name. Just rejects the specific
         * wildcard addresses that can't mean anything as a callback. */
        add_error(out, LISTENER_CONFIG_FIELD_CALLBACK_HOST, "Callback host cannot be a wildcard address");
    }

    if (config->type == LISTENER_TYPE_HTTPS) {
        if (!config->cert_path || !*config->cert_path) {
            add_error(out, LISTENER_CONFIG_FIELD_CERT_PATH, "Certificate path is required for HTTPS");
        }
        if (!config->key_path || !*config->key_path) {
            add_error(out, LISTENER_CONFIG_FIELD_KEY_PATH, "Private key path is required for HTTPS");
        }
    }

    int count = object_registry_listener_count(registry);
    for (int i = 0; i < count; i++) {
        const Listener *existing = object_registry_get_listener_at(registry, i);
        if (config->name && existing->config.name && strcmp(config->name, existing->config.name) == 0) {
            char message[128];
            snprintf(message, sizeof(message), "A listener named '%s' already exists", config->name);
            add_error(out, LISTENER_CONFIG_FIELD_NAME, message);
            break;
        }
    }
    for (int i = 0; i < count; i++) {
        const Listener *existing = object_registry_get_listener_at(registry, i);
        if (config->bind_address && existing->config.bind_address && config->port == existing->config.port &&
            strcmp(config->bind_address, existing->config.bind_address) == 0) {
            char message[128];
            snprintf(message, sizeof(message), "A listener already exists on %s:%u", config->bind_address,
                     config->port);
            add_error(out, LISTENER_CONFIG_FIELD_ENDPOINT, message);
            break;
        }
    }

    return out->error_count == 0;
}

/* Cap chosen generously above any realistic number of concurrently
 * starting/running listeners. */
#define LISTENER_MANAGER_MAX_WORKERS 256

/* Tracks the worker thread behind each currently-starting-or-running
 * listener - implementation-internal bookkeeping that intentionally
 * never touches ObjectRegistry's Listener/ListenerRuntime types (the
 * listening socket's fd never needs to be GUI/registry-visible; see the
 * Phase 3 plan). Dense array + linear search by id, same shape as
 * ObjectRegistry's own storage. */
typedef struct WorkerEntry {
    uint64_t listener_id;
    TcpWorker worker;
    bool restart_pending; /* set by listener_manager_restart() when it stops a RUNNING
                            * listener on its way to starting it again; consumed by
                            * process_events()'s STOPPED case right before this entry
                            * is reaped */
    bool remove_pending;  /* same shape, set by listener_manager_remove() when it stops
                            * a RUNNING listener on its way to removing it; takes
                            * precedence over restart_pending if somehow both are set */
} WorkerEntry;

/* registry and events are borrowed - see the header. */
struct ListenerManager {
    ObjectRegistry *registry;
    EventQueue *events;
    WorkerEntry workers[LISTENER_MANAGER_MAX_WORKERS];
    int worker_count;
};

ListenerManager *listener_manager_create(ObjectRegistry *registry, EventQueue *events) {
    ListenerManager *manager = malloc(sizeof(ListenerManager));
    manager->registry = registry;
    manager->events = events;
    manager->worker_count = 0;
    return manager;
}

void listener_manager_destroy(ListenerManager *manager) {
    if (!manager) {
        return;
    }
    /* Defensive: if the caller destroys the manager without stopping
     * every listener first, still signal and join every tracked worker
     * rather than orphaning its thread (a joinable pthread never
     * detached or joined leaks its resources). */
    for (int i = 0; i < manager->worker_count; i++) {
        tcp_worker_signal_stop(&manager->workers[i].worker);
        tcp_worker_join(&manager->workers[i].worker);
    }
    free(manager);
}

static int find_worker_index(ListenerManager *manager, uint64_t listener_id) {
    for (int i = 0; i < manager->worker_count; i++) {
        if (manager->workers[i].listener_id == listener_id) {
            return i;
        }
    }
    return -1;
}

static void reap_worker(ListenerManager *manager, uint64_t listener_id) {
    int index = find_worker_index(manager, listener_id);
    if (index < 0) {
        return;
    }
    tcp_worker_join(&manager->workers[index].worker);
    for (int i = index; i < manager->worker_count - 1; i++) {
        manager->workers[i] = manager->workers[i + 1];
    }
    manager->worker_count--;
}

static void push_event(ListenerManager *manager, ListenerEventType type, uint64_t id, const char *message) {
    ListenerEvent event = {0};
    event.type = type;
    event.object_id = id;
    if (message) {
        snprintf(event.message, sizeof(event.message), "%s", message);
    }
    event_queue_push(manager->events, event);
}

/* Builds a throwaway ManagedObject snapshot to reuse Phase 1's
 * object_can_* predicates, which take a ManagedObject rather than a
 * bare Listener. */
static bool listener_predicate(const ObjectRegistry *registry, uint64_t id, bool (*predicate)(const ManagedObject *)) {
    const Listener *listener = object_registry_get_listener(registry, id);
    if (!listener) {
        return false;
    }
    ManagedObject obj;
    obj.type = MANAGED_OBJECT_LISTENER;
    obj.listener = *listener;
    return predicate(&obj);
}

uint64_t listener_manager_create_listener(ListenerManager *manager, ListenerConfig config,
                                           ListenerConfigValidation *out_validation) {
    ListenerConfigValidation validation;
    bool valid = listener_config_validate(manager->registry, &config, &validation);
    if (out_validation) {
        *out_validation = validation;
    }
    if (!valid) {
        return 0;
    }

    uint64_t id = object_registry_add_listener(manager->registry, config);
    if (id == 0) {
        return 0;
    }
    push_event(manager, LISTENER_EVENT_CREATED, id, NULL);
    return id;
}

int listener_manager_start_async(ListenerManager *manager, uint64_t id) {
    if (!listener_predicate(manager->registry, id, object_can_start)) {
        return -1;
    }
    const Listener *listener = object_registry_get_listener(manager->registry, id);
    const char *bind_address = listener->config.bind_address;
    uint16_t port = listener->config.port;

    ListenerRuntime *runtime = object_registry_get_listener_runtime_mut(manager->registry, id);
    runtime->state = LISTENER_STATE_STARTING;
    push_event(manager, LISTENER_EVENT_STARTING, id, NULL);

    if (manager->worker_count >= LISTENER_MANAGER_MAX_WORKERS) {
        listener_manager_report_start_result(manager, id, false,
                                              "too many concurrently starting/running listeners");
        return 0;
    }

    TcpWorker worker;
    if (tcp_worker_start(&worker, manager->events, id, bind_address, port) != 0) {
        listener_manager_report_start_result(manager, id, false, "failed to start worker thread");
        return 0;
    }

    manager->workers[manager->worker_count].listener_id = id;
    manager->workers[manager->worker_count].worker = worker;
    manager->workers[manager->worker_count].restart_pending = false;
    manager->workers[manager->worker_count].remove_pending = false;
    manager->worker_count++;
    return 0;
}

int listener_manager_stop(ListenerManager *manager, uint64_t id) {
    if (!listener_predicate(manager->registry, id, object_can_stop)) {
        return -1;
    }
    ListenerRuntime *runtime = object_registry_get_listener_runtime_mut(manager->registry, id);
    runtime->state = LISTENER_STATE_STOPPING;
    push_event(manager, LISTENER_EVENT_STOPPING, id, NULL);

    int index = find_worker_index(manager, id);
    if (index >= 0) {
        tcp_worker_signal_stop(&manager->workers[index].worker);
    }
    return 0;
}

int listener_manager_restart(ListenerManager *manager, uint64_t id) {
    if (!listener_predicate(manager->registry, id, object_can_restart)) {
        return -1;
    }
    const Listener *listener = object_registry_get_listener(manager->registry, id);
    if (listener->runtime.state == LISTENER_STATE_RUNNING) {
        int index = find_worker_index(manager, id);
        if (index >= 0) {
            manager->workers[index].restart_pending = true;
        }
        /* Re-validates object_can_stop(), which RUNNING already satisfies. */
        return listener_manager_stop(manager, id);
    }
    /* STOPPED or ERROR: no stop needed, just start again. */
    return listener_manager_start_async(manager, id);
}

/* Not gated by object_can_*() - there isn't one for removal, it's
 * always available. A Listener owns no socket itself (tcp_worker.c
 * owns and closes its own listening socket internally), so removing
 * one is safe even mid-transition; RUNNING is guarded here purely for
 * operator safety (don't silently delete something doing active work),
 * not memory safety. */
int listener_manager_remove(ListenerManager *manager, uint64_t id) {
    const Listener *listener = object_registry_get_listener(manager->registry, id);
    if (!listener) {
        return -1;
    }
    if (listener->runtime.state == LISTENER_STATE_RUNNING) {
        int index = find_worker_index(manager, id);
        if (index >= 0) {
            manager->workers[index].remove_pending = true;
        }
        return listener_manager_stop(manager, id);
    }
    return object_registry_remove(manager->registry, id);
}

/* Transition-only: applies the outcome without pushing an event. Used
 * both by the public report_start_result (which pushes the event
 * itself, since nothing else has) and by process_events (which is
 * reacting to an event a worker already pushed - pushing another would
 * duplicate it). Returns 0 on success, -1 if id isn't currently
 * STARTING. */
static int apply_start_result(ListenerManager *manager, uint64_t id, bool success, const char *error_message) {
    const Listener *listener = object_registry_get_listener(manager->registry, id);
    if (!listener || listener->runtime.state != LISTENER_STATE_STARTING) {
        return -1;
    }

    ListenerRuntime *runtime = object_registry_get_listener_runtime_mut(manager->registry, id);
    free(runtime->last_error);
    if (success) {
        runtime->state = LISTENER_STATE_RUNNING;
        runtime->started_at = time(NULL);
        runtime->last_error = NULL;
    } else {
        runtime->state = LISTENER_STATE_ERROR;
        runtime->last_error = error_message ? strdup(error_message) : NULL;
    }
    return 0;
}

/* Transition-only counterpart to apply_start_result - see that comment. */
static int apply_stop_result(ListenerManager *manager, uint64_t id) {
    const Listener *listener = object_registry_get_listener(manager->registry, id);
    if (!listener || listener->runtime.state != LISTENER_STATE_STOPPING) {
        return -1;
    }
    ListenerRuntime *runtime = object_registry_get_listener_runtime_mut(manager->registry, id);
    runtime->state = LISTENER_STATE_STOPPED;
    return 0;
}

int listener_manager_report_start_result(ListenerManager *manager, uint64_t id, bool success,
                                          const char *error_message) {
    if (apply_start_result(manager, id, success, error_message) != 0) {
        return -1;
    }
    push_event(manager, success ? LISTENER_EVENT_STARTED : LISTENER_EVENT_START_FAILED, id, error_message);
    return 0;
}

int listener_manager_report_stop_result(ListenerManager *manager, uint64_t id) {
    if (apply_stop_result(manager, id) != 0) {
        return -1;
    }
    push_event(manager, LISTENER_EVENT_STOPPED, id, NULL);
    return 0;
}

int listener_manager_process_events(ListenerManager *manager, ListenerEvent *out, int max_out) {
    int n = 0;
    ListenerEvent event;
    while ((out == NULL || n < max_out) && event_queue_try_pop(manager->events, &event)) {
        switch (event.type) {
            case LISTENER_EVENT_STARTED:
                apply_start_result(manager, event.object_id, true, NULL);
                break;
            case LISTENER_EVENT_START_FAILED:
                apply_start_result(manager, event.object_id, false, event.message);
                reap_worker(manager, event.object_id);
                break;
            case LISTENER_EVENT_STOPPED: {
                apply_stop_result(manager, event.object_id);
                /* Captured before reap_worker() removes the entry
                 * (and its restart_pending/remove_pending flags along
                 * with it). */
                int index = find_worker_index(manager, event.object_id);
                bool restart_pending = index >= 0 && manager->workers[index].restart_pending;
                bool remove_pending = index >= 0 && manager->workers[index].remove_pending;
                reap_worker(manager, event.object_id);
                if (remove_pending) {
                    object_registry_remove(manager->registry, event.object_id);
                } else if (restart_pending) {
                    listener_manager_start_async(manager, event.object_id);
                }
                break;
            }
            case LISTENER_EVENT_CONNECTION_OPENED:
                /* object_id stays the *listener's* id (matches how the
                 * worker pushed it - see tcp_worker.c); the new
                 * Connection's id, needed by a caller wiring up
                 * ConnectionManager, goes in the scratch `sequence`
                 * field instead, since nothing else uses it for this
                 * event type. */
                event.sequence =
                    object_registry_add_connection(manager->registry, event.object_id, event.remote_host,
                                                    event.remote_port, event.socket_fd);
                break;
            default:
                break; /* CREATED/STARTING/STOPPING/TEST already applied synchronously - nothing to do */
        }
        if (out) {
            out[n] = event;
        }
        n++;
    }
    return n;
}
