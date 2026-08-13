#ifndef WORKBENCH_OBJECT_PREDICATES_H
#define WORKBENCH_OBJECT_PREDICATES_H

#include <stdbool.h>

#include "managed_object.h"

/*
 * State-derived command availability, driving GUI enable/disable state
 * in later phases. Each predicate covers both ManagedObject types, since
 * "can I start this" etc. means something different (or nothing) for a
 * Listener versus a Connection - see object_predicates.c for the full
 * state matrix and rationale.
 */

bool object_can_start(const ManagedObject *obj);
bool object_can_stop(const ManagedObject *obj);
bool object_can_restart(const ManagedObject *obj);
bool object_can_open_terminal(const ManagedObject *obj);
bool object_can_wait_for_reconnection(const ManagedObject *obj);

#endif /* WORKBENCH_OBJECT_PREDICATES_H */
