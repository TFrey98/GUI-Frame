/*
 * Exercises ObjectRegistry as the sole owner of Listeners and
 * Connections: id allocation (one shared, collision-free space across
 * both object kinds), config/field round-tripping, listener->connection
 * queries, and that removal is unconditional and non-cascading (removing
 * a listener doesn't touch connections that still reference its id -
 * that's a later phase's business rule to enforce, not this storage
 * layer's).
 */
#include <stdio.h>
#include <string.h>

#include "listeners/object_registry.h"

static ListenerConfig make_config(const char *name, const char *bind_address, uint16_t port,
                                   const char *callback_host) {
    ListenerConfig config = {0};
    config.name = strdup(name);
    config.type = LISTENER_TYPE_REVERSE_TCP;
    config.bind_address = strdup(bind_address);
    config.port = port;
    config.callback_host = strdup(callback_host);
    config.cert_path = NULL;
    config.key_path = NULL;
    return config;
}

int main(void) {
    int status = 0;
    ObjectRegistry *registry = object_registry_create();

    uint64_t listener_a = object_registry_add_listener(registry, make_config("A", "0.0.0.0", 4444, "203.0.113.1"));
    uint64_t listener_b = object_registry_add_listener(registry, make_config("B", "0.0.0.0", 4445, "203.0.113.1"));
    uint64_t listener_c = object_registry_add_listener(registry, make_config("C", "0.0.0.0", 4446, "203.0.113.1"));

    if (listener_a == 0 || listener_b == 0 || listener_c == 0 || listener_a == listener_b ||
        listener_b == listener_c || listener_a == listener_c) {
        fprintf(stderr, "object_registry_test: expected 3 distinct nonzero listener ids\n");
        status = 1;
    }

    const Listener *la = object_registry_get_listener(registry, listener_a);
    if (!la || strcmp(la->config.name, "A") != 0 || la->config.port != 4444 ||
        la->runtime.state != LISTENER_STATE_CONFIGURED) {
        fprintf(stderr, "object_registry_test: listener A's fields were not preserved / wrong initial state\n");
        status = 1;
    }

    uint64_t conn_a1 = object_registry_add_connection(registry, listener_a, "198.51.100.10", 51000, -1, NULL);
    uint64_t conn_a2 = object_registry_add_connection(registry, listener_a, "198.51.100.11", 51001, -1, NULL);
    uint64_t conn_b1 = object_registry_add_connection(registry, listener_b, "198.51.100.12", 51002, -1, NULL);

    if (conn_a1 == 0 || conn_a2 == 0 || conn_b1 == 0 || conn_a1 == listener_a || conn_a1 == listener_b ||
        conn_a1 == listener_c) {
        fprintf(stderr, "object_registry_test: connection ids collided with listener ids or were zero\n");
        status = 1;
    }

    const Connection *ca1 = object_registry_get_connection(registry, conn_a1);
    if (!ca1 || strcmp(ca1->remote_host, "198.51.100.10") != 0 || ca1->remote_port != 51000 ||
        ca1->listener_id != listener_a || ca1->state != CONNECTION_STATE_CONNECTED) {
        fprintf(stderr, "object_registry_test: connection A1's fields were not preserved\n");
        status = 1;
    }

    const Connection *for_a[8];
    int count_a = object_registry_list_connections_for_listener(registry, listener_a, for_a, 8);
    if (count_a != 2 || (for_a[0]->id != conn_a1 && for_a[1]->id != conn_a1) ||
        (for_a[0]->id != conn_a2 && for_a[1]->id != conn_a2)) {
        fprintf(stderr, "object_registry_test: expected listener A to own exactly connections A1 and A2\n");
        status = 1;
    }

    const Connection *for_b[8];
    int count_b = object_registry_list_connections_for_listener(registry, listener_b, for_b, 8);
    if (count_b != 1 || for_b[0]->id != conn_b1) {
        fprintf(stderr, "object_registry_test: expected listener B to own exactly connection B1\n");
        status = 1;
    }

    const Connection *for_c[8];
    int count_c = object_registry_list_connections_for_listener(registry, listener_c, for_c, 8);
    if (count_c != 0) {
        fprintf(stderr, "object_registry_test: expected listener C to own no connections, got %d\n", count_c);
        status = 1;
    }

    if (object_registry_remove(registry, conn_a2) != 0) {
        fprintf(stderr, "object_registry_test: removing connection A2 should have succeeded\n");
        status = 1;
    }
    count_a = object_registry_list_connections_for_listener(registry, listener_a, for_a, 8);
    if (count_a != 1 || for_a[0]->id != conn_a1) {
        fprintf(stderr, "object_registry_test: listener A should own only A1 after removing A2\n");
        status = 1;
    }
    if (!object_registry_get_listener(registry, listener_a)) {
        fprintf(stderr, "object_registry_test: removing a connection should not affect its listener\n");
        status = 1;
    }

    if (object_registry_remove(registry, listener_b) != 0) {
        fprintf(stderr, "object_registry_test: removing listener B should have succeeded\n");
        status = 1;
    }
    if (object_registry_get_listener(registry, listener_b) != NULL) {
        fprintf(stderr, "object_registry_test: listener B should be gone after removal\n");
        status = 1;
    }
    /* No cascade: B1 still exists and still points at B's (now-removed) id. */
    if (!object_registry_get_connection(registry, conn_b1)) {
        fprintf(stderr, "object_registry_test: removing a listener should not cascade-remove its connections\n");
        status = 1;
    }

    if (object_registry_remove(registry, 999999) != -1) {
        fprintf(stderr, "object_registry_test: removing a never-issued id should fail\n");
        status = 1;
    }
    if (object_registry_find(registry, 999999) != NULL) {
        fprintf(stderr, "object_registry_test: find() on a never-issued id should return NULL\n");
        status = 1;
    }

    if (object_registry_listener_count(registry) != 2) {
        fprintf(stderr, "object_registry_test: expected 2 listeners remaining, got %d\n",
                object_registry_listener_count(registry));
        status = 1;
    }

    object_registry_destroy(registry);

    if (status == 0) {
        printf("object_registry_test: id allocation, queries, and non-cascading removal verified\n");
    }
    return status;
}
