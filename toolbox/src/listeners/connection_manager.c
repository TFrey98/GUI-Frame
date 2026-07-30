#include "connection_manager.h"

#include <openssl/ssl.h>
#include <stdlib.h>
#include <unistd.h>

#include "connection_worker.h"
#include "object_predicates.h"

/* Cap chosen generously above any realistic number of concurrently
 * live connections for a single operator session. */
#define CONNECTION_MANAGER_MAX_WORKERS 1024

/* Same dense-array-plus-linear-search shape as ListenerManager's
 * WorkerEntry table and ObjectRegistry's own storage. */
typedef struct WorkerEntry {
    uint64_t connection_id;
    ConnectionWorker worker;
    bool remove_pending; /* set by connection_manager_remove() when it disconnects a
                           * CONNECTED connection on its way to removing it; consumed
                           * by handle_closed() right before this entry is spliced out */
} WorkerEntry;

/* registry and events are borrowed - see the header. */
struct ConnectionManager {
    ObjectRegistry *registry;
    EventQueue *events;
    WorkerEntry workers[CONNECTION_MANAGER_MAX_WORKERS];
    int worker_count;
};

ConnectionManager *connection_manager_create(ObjectRegistry *registry, EventQueue *events) {
    ConnectionManager *manager = malloc(sizeof(ConnectionManager));
    manager->registry = registry;
    manager->events = events;
    manager->worker_count = 0;
    return manager;
}

static int find_worker_index(ConnectionManager *manager, uint64_t connection_id) {
    for (int i = 0; i < manager->worker_count; i++) {
        if (manager->workers[i].connection_id == connection_id) {
            return i;
        }
    }
    return -1;
}

/* Drains everything currently in entry's incoming buffer into its
 * connection's TerminalHistory. Shared by connection_manager_drain_data()
 * (every live connection, every tick) and connection_manager_handle_closed()
 * (one last drain before that connection's buffers are destroyed - the
 * worker's last read-into-incoming always happens-before the
 * CONNECTION_CLOSED event that triggers handle_closed, so this is
 * guaranteed to see everything it wrote). */
static void drain_worker_incoming(ConnectionManager *manager, WorkerEntry *entry) {
    Connection *connection = object_registry_get_connection_mut(manager->registry, entry->connection_id);
    if (!connection) {
        return;
    }
    unsigned char chunk[4096];
    size_t n;
    while ((n = byte_buffer_consume(entry->worker.incoming, chunk, sizeof(chunk))) > 0) {
        terminal_history_append(connection->history, chunk, n);
    }
}

void connection_manager_destroy(ConnectionManager *manager) {
    if (!manager) {
        return;
    }
    /* Defensive, same reasoning as ListenerManager's destroy: don't
     * orphan a still-running worker thread if the caller tears down
     * without disconnecting everything first. */
    for (int i = 0; i < manager->worker_count; i++) {
        connection_worker_signal_stop(&manager->workers[i].worker);
        connection_worker_join(&manager->workers[i].worker);
    }
    free(manager);
}

static void push_connection_closed(ConnectionManager *manager, uint64_t connection_id) {
    ListenerEvent event = {0};
    event.type = LISTENER_EVENT_CONNECTION_CLOSED;
    event.object_id = connection_id;
    event_queue_push(manager->events, event);
}

void connection_manager_handle_closed(ConnectionManager *manager, uint64_t connection_id) {
    int index = find_worker_index(manager, connection_id);
    bool remove_pending = false;
    if (index >= 0) {
        remove_pending = manager->workers[index].remove_pending;
        drain_worker_incoming(manager, &manager->workers[index]);
        connection_worker_join(&manager->workers[index].worker);
        for (int i = index; i < manager->worker_count - 1; i++) {
            manager->workers[i] = manager->workers[i + 1];
        }
        manager->worker_count--;
    }

    if (remove_pending) {
        object_registry_remove(manager->registry, connection_id);
        return;
    }

    Connection *connection = object_registry_get_connection_mut(manager->registry, connection_id);
    if (connection) {
        connection->state = CONNECTION_STATE_DISCONNECTED;
    }
}

int connection_manager_start_worker(ConnectionManager *manager, uint64_t connection_id) {
    const Connection *connection = object_registry_get_connection(manager->registry, connection_id);
    if (!connection) {
        return -1;
    }
    int socket_fd = connection->socket_fd;
    void *tls = connection->tls;

    if (manager->worker_count >= CONNECTION_MANAGER_MAX_WORKERS) {
        if (tls) {
            SSL_free((SSL *)tls);
        }
        close(socket_fd);
        connection_manager_handle_closed(manager, connection_id);
        push_connection_closed(manager, connection_id);
        return -1;
    }

    ConnectionWorker worker;
    if (connection_worker_start(&worker, manager->events, connection_id, socket_fd, tls) != 0) {
        /* ownership was never transferred since start failed */
        if (tls) {
            SSL_free((SSL *)tls);
        }
        close(socket_fd);
        connection_manager_handle_closed(manager, connection_id);
        push_connection_closed(manager, connection_id);
        return -1;
    }

    manager->workers[manager->worker_count].connection_id = connection_id;
    manager->workers[manager->worker_count].worker = worker;
    manager->workers[manager->worker_count].remove_pending = false;
    manager->worker_count++;
    return 0;
}

int connection_manager_disconnect(ConnectionManager *manager, uint64_t connection_id) {
    const Connection *connection = object_registry_get_connection(manager->registry, connection_id);
    if (!connection) {
        return -1;
    }
    ManagedObject obj;
    obj.type = MANAGED_OBJECT_CONNECTION;
    obj.connection = *connection;
    if (!object_can_stop(&obj)) {
        return -1;
    }

    int index = find_worker_index(manager, connection_id);
    if (index < 0) {
        return -1; /* CONNECTED should always have a worker; defensive */
    }
    connection_worker_signal_stop(&manager->workers[index].worker);
    return 0;
}

/* Not gated by object_can_*() - removal is always available. Guarded
 * for CONNECTED: the worker thread is still concurrently touching
 * Connection.socket_fd, so removal must wait for
 * connection_manager_handle_closed() to actually join it - the same
 * safe point a real disconnect uses. */
int connection_manager_remove(ConnectionManager *manager, uint64_t connection_id) {
    const Connection *connection = object_registry_get_connection(manager->registry, connection_id);
    if (!connection) {
        return -1;
    }
    if (connection->state == CONNECTION_STATE_CONNECTED) {
        int index = find_worker_index(manager, connection_id);
        if (index >= 0) {
            manager->workers[index].remove_pending = true;
        }
        return connection_manager_disconnect(manager, connection_id);
    }
    return object_registry_remove(manager->registry, connection_id);
}

int connection_manager_send(ConnectionManager *manager, uint64_t connection_id, const void *data, size_t len) {
    int index = find_worker_index(manager, connection_id);
    if (index < 0) {
        return -1;
    }
    byte_buffer_append(manager->workers[index].worker.outgoing, data, len);
    connection_worker_notify_outgoing(&manager->workers[index].worker);
    return 0;
}

void connection_manager_drain_data(ConnectionManager *manager) {
    for (int i = 0; i < manager->worker_count; i++) {
        drain_worker_incoming(manager, &manager->workers[i]);
    }
}
