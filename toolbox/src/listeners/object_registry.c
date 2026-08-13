#include "object_registry.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Cap chosen generously above any realistic number of concurrently
 * tracked listeners+connections for a single operator session; revisit
 * if that assumption turns out wrong. */
#define OBJECT_REGISTRY_MAX_OBJECTS 1024

/* Dense array + linear search by id + shift-on-remove, same shape as
 * Workspace's tab storage (src/core/workspace.c) scaled up for a
 * plausibly larger number of connections. Not thread-safe - see the
 * header comment for why that's fine here. */
struct ObjectRegistry {
    ManagedObject *objects[OBJECT_REGISTRY_MAX_OBJECTS];
    int count;
    uint64_t next_id; /* shared id space across Listeners and Connections */
};

ObjectRegistry *object_registry_create(void) {
    ObjectRegistry *registry = calloc(1, sizeof(ObjectRegistry));
    registry->next_id = 1;
    return registry;
}

static void free_object_contents(ManagedObject *obj) {
    if (obj->type == MANAGED_OBJECT_LISTENER) {
        free(obj->listener.config.name);
        free(obj->listener.config.bind_address);
        free(obj->listener.config.callback_host);
        free(obj->listener.config.cert_path);
        free(obj->listener.config.key_path);
        free(obj->listener.config.url_path);
        free(obj->listener.config.host_header);
        free(obj->listener.runtime.last_error);
    } else {
        free(obj->connection.remote_host);
        if (obj->connection.socket_fd >= 0) {
            close(obj->connection.socket_fd);
        }
        terminal_history_destroy(obj->connection.history);
    }
}

void object_registry_destroy(ObjectRegistry *registry) {
    if (!registry) {
        return;
    }
    for (int i = 0; i < registry->count; i++) {
        free_object_contents(registry->objects[i]);
        free(registry->objects[i]);
    }
    free(registry);
}

static int find_index(const ObjectRegistry *registry, uint64_t id) {
    for (int i = 0; i < registry->count; i++) {
        if (managed_object_id(registry->objects[i]) == id) {
            return i;
        }
    }
    return -1;
}

uint64_t object_registry_add_listener(ObjectRegistry *registry, ListenerConfig config) {
    if (registry->count >= OBJECT_REGISTRY_MAX_OBJECTS) {
        return 0; /* full - ownership of config's fields was not taken; caller still owns them */
    }

    ManagedObject *obj = malloc(sizeof(ManagedObject));
    obj->type = MANAGED_OBJECT_LISTENER;
    obj->listener.id = registry->next_id++;
    obj->listener.config = config;
    obj->listener.runtime.state = LISTENER_STATE_CONFIGURED;
    obj->listener.runtime.started_at = 0;
    obj->listener.runtime.last_error = NULL;

    registry->objects[registry->count++] = obj;
    return obj->listener.id;
}

uint64_t object_registry_add_connection(ObjectRegistry *registry, uint64_t listener_id, const char *remote_host,
                                         uint16_t remote_port, int socket_fd, void *tls) {
    if (registry->count >= OBJECT_REGISTRY_MAX_OBJECTS) {
        return 0;
    }

    ManagedObject *obj = malloc(sizeof(ManagedObject));
    obj->type = MANAGED_OBJECT_CONNECTION;
    obj->connection.id = registry->next_id++;
    obj->connection.listener_id = listener_id;
    obj->connection.state = CONNECTION_STATE_CONNECTED;
    obj->connection.remote_host = strdup(remote_host ? remote_host : "");
    obj->connection.remote_port = remote_port;
    obj->connection.connected_at = time(NULL);
    obj->connection.socket_fd = socket_fd;
    obj->connection.tls = tls;
    obj->connection.history = terminal_history_create("connection", obj->connection.id);
    obj->connection.capturing_output = false;

    registry->objects[registry->count++] = obj;
    return obj->connection.id;
}

int object_registry_remove(ObjectRegistry *registry, uint64_t id) {
    int index = find_index(registry, id);
    if (index < 0) {
        return -1;
    }

    free_object_contents(registry->objects[index]);
    free(registry->objects[index]);

    for (int i = index; i < registry->count - 1; i++) {
        registry->objects[i] = registry->objects[i + 1];
    }
    registry->count--;
    return 0;
}

const ManagedObject *object_registry_find(const ObjectRegistry *registry, uint64_t id) {
    int index = find_index(registry, id);
    return index < 0 ? NULL : registry->objects[index];
}

const Listener *object_registry_get_listener(const ObjectRegistry *registry, uint64_t id) {
    const ManagedObject *obj = object_registry_find(registry, id);
    return (obj && obj->type == MANAGED_OBJECT_LISTENER) ? &obj->listener : NULL;
}

const Connection *object_registry_get_connection(const ObjectRegistry *registry, uint64_t id) {
    const ManagedObject *obj = object_registry_find(registry, id);
    return (obj && obj->type == MANAGED_OBJECT_CONNECTION) ? &obj->connection : NULL;
}

Connection *object_registry_get_connection_mut(ObjectRegistry *registry, uint64_t id) {
    int index = find_index(registry, id);
    if (index < 0 || registry->objects[index]->type != MANAGED_OBJECT_CONNECTION) {
        return NULL;
    }
    return &registry->objects[index]->connection;
}

ListenerRuntime *object_registry_get_listener_runtime_mut(ObjectRegistry *registry, uint64_t id) {
    int index = find_index(registry, id);
    if (index < 0 || registry->objects[index]->type != MANAGED_OBJECT_LISTENER) {
        return NULL;
    }
    return &registry->objects[index]->listener.runtime;
}

int object_registry_listener_count(const ObjectRegistry *registry) {
    int count = 0;
    for (int i = 0; i < registry->count; i++) {
        if (registry->objects[i]->type == MANAGED_OBJECT_LISTENER) {
            count++;
        }
    }
    return count;
}

const Listener *object_registry_get_listener_at(const ObjectRegistry *registry, int index) {
    if (index < 0) {
        return NULL;
    }
    int seen = 0;
    for (int i = 0; i < registry->count; i++) {
        if (registry->objects[i]->type != MANAGED_OBJECT_LISTENER) {
            continue;
        }
        if (seen == index) {
            return &registry->objects[i]->listener;
        }
        seen++;
    }
    return NULL;
}

int object_registry_list_connections_for_listener(const ObjectRegistry *registry, uint64_t listener_id,
                                                    const Connection **out, int max_out) {
    int written = 0;
    for (int i = 0; i < registry->count && written < max_out; i++) {
        const ManagedObject *obj = registry->objects[i];
        if (obj->type == MANAGED_OBJECT_CONNECTION && obj->connection.listener_id == listener_id) {
            out[written++] = &obj->connection;
        }
    }
    return written;
}
