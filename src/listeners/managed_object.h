#ifndef WORKBENCH_MANAGED_OBJECT_H
#define WORKBENCH_MANAGED_OBJECT_H

#include "connection.h"
#include "listener.h"

/*
 * Tagged union letting ObjectRegistry store and query Listeners and
 * Connections uniformly (one id space, one predicate API - see
 * object_predicates.h) without either domain needing to know about the
 * other's shape.
 */
typedef enum ManagedObjectType {
    MANAGED_OBJECT_LISTENER,
    MANAGED_OBJECT_CONNECTION,
} ManagedObjectType;

typedef struct ManagedObject {
    ManagedObjectType type;
    union {
        Listener listener;
        Connection connection;
    };
} ManagedObject;

/* Returns listener.id or connection.id depending on obj->type. */
uint64_t managed_object_id(const ManagedObject *obj);

#endif /* WORKBENCH_MANAGED_OBJECT_H */
