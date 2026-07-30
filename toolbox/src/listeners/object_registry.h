#ifndef TOOLBOX_OBJECT_REGISTRY_H
#define TOOLBOX_OBJECT_REGISTRY_H

#include "managed_object.h"

/*
 * Sole owner of every Listener and Connection. Nothing else constructs
 * or frees these objects - callers get ids back and resolve through
 * this registry every time they need the current data, never a cached
 * pointer (a later phase's ListenerManager/ConnectionManager will add
 * state-machine/validation logic on top of this storage layer, and the
 * GUI will read through it every frame).
 *
 * Not thread-safe: only the GUI thread is ever expected to touch it.
 * Worker threads (once they exist, from the networking phases onward)
 * communicate exclusively through EventQueue - see event_queue.h.
 */
typedef struct ObjectRegistry ObjectRegistry;

ObjectRegistry *object_registry_create(void);
void object_registry_destroy(ObjectRegistry *registry);

/* Takes ownership of config's heap-allocated fields (name, bind_address,
 * callback_host, cert_path, key_path); constructs the Listener in
 * LISTENER_STATE_CONFIGURED. Returns the new id, or 0 if the registry is
 * full. */
uint64_t object_registry_add_listener(ObjectRegistry *registry, ListenerConfig config);

/* Constructs the Connection in CONNECTION_STATE_CONNECTED with
 * connected_at set to now. remote_host is copied. socket_fd is taken
 * as-is (pass -1 if there's no real socket, e.g. in a test); it is
 * closed automatically when the Connection is removed/the registry is
 * destroyed. tls is stored as-is (pass NULL for a non-TLS connection) -
 * unlike socket_fd, this registry never frees it; see connection.h's
 * comment on Connection.tls for why. Returns the new id, or 0 if the
 * registry is full. */
uint64_t object_registry_add_connection(ObjectRegistry *registry, uint64_t listener_id,
                                         const char *remote_host, uint16_t remote_port, int socket_fd, void *tls);

/* Unconditional removal: frees the object's owned strings (and closes a
 * Connection's socket_fd, if >= 0) and drops it from the registry,
 * regardless of its current state and regardless of whether a
 * Connection still references a removed Listener's id (no cascade).
 * Business rules for *when* removal should be allowed (e.g. confirming
 * before removing a running object) belong to later phases' managers or
 * the GUI, not this storage layer. Returns 0 on success, -1 if no such
 * id exists. */
int object_registry_remove(ObjectRegistry *registry, uint64_t id);

const ManagedObject *object_registry_find(const ObjectRegistry *registry, uint64_t id);
const Listener *object_registry_get_listener(const ObjectRegistry *registry, uint64_t id);
const Connection *object_registry_get_connection(const ObjectRegistry *registry, uint64_t id);

/* Mutable access to a Connection - for ConnectionManager's exclusive
 * use, the sole driver of connection state transitions, to set .state
 * and append to .history. Registry storage still owns the memory.
 * Returns NULL if no such connection exists. */
Connection *object_registry_get_connection_mut(ObjectRegistry *registry, uint64_t id);

/* Mutable access to a Listener's runtime fields (state, started_at,
 * last_error) - for ListenerManager's exclusive use as the sole driver
 * of listener state transitions. Registry storage still owns the
 * memory and frees it on remove/destroy; this just hands out a pointer
 * into it. Returns NULL if no such listener exists. */
ListenerRuntime *object_registry_get_listener_runtime_mut(ObjectRegistry *registry, uint64_t id);

int object_registry_listener_count(const ObjectRegistry *registry);
const Listener *object_registry_get_listener_at(const ObjectRegistry *registry, int index);

/* Writes up to max_out Connections belonging to listener_id into out, in
 * registry order. Returns the number written. */
int object_registry_list_connections_for_listener(const ObjectRegistry *registry, uint64_t listener_id,
                                                    const Connection **out, int max_out);

#endif /* TOOLBOX_OBJECT_REGISTRY_H */
