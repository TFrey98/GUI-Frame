#include "managed_object.h"

uint64_t managed_object_id(const ManagedObject *obj) {
    return obj->type == MANAGED_OBJECT_LISTENER ? obj->listener.id : obj->connection.id;
}
