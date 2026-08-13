#ifndef TOOLBOX_LISTENER_MANAGER_H
#define TOOLBOX_LISTENER_MANAGER_H

#include <stdbool.h>

#include "event_queue.h"
#include "object_registry.h"

/*
 * Validation
 */

typedef enum ListenerConfigField {
    LISTENER_CONFIG_FIELD_NAME,
    LISTENER_CONFIG_FIELD_TYPE,
    LISTENER_CONFIG_FIELD_PORT,
    LISTENER_CONFIG_FIELD_BIND_ADDRESS,
    LISTENER_CONFIG_FIELD_CALLBACK_HOST,
    LISTENER_CONFIG_FIELD_CERT_PATH,
    LISTENER_CONFIG_FIELD_KEY_PATH,
    LISTENER_CONFIG_FIELD_URL_PATH,
    LISTENER_CONFIG_FIELD_HOST_HEADER,
    LISTENER_CONFIG_FIELD_ENDPOINT, /* whole-config uniqueness, not one input field */
} ListenerConfigField;

#define LISTENER_CONFIG_MAX_ERRORS 8

typedef struct ListenerConfigError {
    ListenerConfigField field;
    char message[128];
} ListenerConfigError;

typedef struct ListenerConfigValidation {
    int error_count;
    ListenerConfigError errors[LISTENER_CONFIG_MAX_ERRORS];
} ListenerConfigValidation;

/* Validates config in isolation plus name/endpoint uniqueness against
 * listeners already in registry. Doesn't mutate config or registry.
 * Collects every applicable error (not just the first) into out. Returns
 * true iff out->error_count == 0. */
bool listener_config_validate(const ObjectRegistry *registry, const ListenerConfig *config,
                               ListenerConfigValidation *out);

/*
 * ListenerManager: validation + the CONFIGURED -> STARTING -> RUNNING ->
 * STOPPING -> STOPPED/ERROR state machine + event emission, backed by a
 * real reverse-TCP worker thread per listener (see tcp_worker.h).
 * start_async/stop only ever transition state and emit events
 * synchronously themselves (STARTING/STOPPING) - the worker-reported
 * outcome (STARTED/START_FAILED/STOPPED) is applied later by
 * listener_manager_process_events(), since workers never touch
 * ObjectRegistry directly. report_start_result/report_stop_result apply
 * that same outcome directly; they exist for tests/manual use and are
 * the internal seam process_events itself is built on.
 */
typedef struct ListenerManager ListenerManager;

/* registry and events are borrowed, not owned - both must outlive the
 * manager. */
ListenerManager *listener_manager_create(ObjectRegistry *registry, EventQueue *events);
void listener_manager_destroy(ListenerManager *manager);

/* Runs listener_config_validate(); on success, stores config via
 * object_registry_add_listener() (which takes ownership of its heap
 * fields) as CONFIGURED and pushes LISTENER_EVENT_CREATED, returning the
 * new id. On validation failure, returns 0 and config's fields are NOT
 * taken - the caller still owns and must free them. out_validation, if
 * non-NULL, is always filled in either way. */
uint64_t listener_manager_create_listener(ListenerManager *manager, ListenerConfig config,
                                           ListenerConfigValidation *out_validation);

/* Requires object_can_start(); returns -1 and does nothing otherwise.
 * Transitions -> STARTING, pushes LISTENER_EVENT_STARTING, and spawns a
 * worker thread that binds/listens in the background (see tcp_worker.h).
 * The actual outcome (RUNNING or ERROR) is applied later, when
 * process_events() drains the worker's STARTED/START_FAILED event -
 * this call returns before that's known. Returns 0. */
int listener_manager_start_async(ListenerManager *manager, uint64_t id);

/* Requires object_can_stop(); returns -1 and does nothing otherwise.
 * Transitions RUNNING -> STOPPING, pushes LISTENER_EVENT_STOPPING, and
 * signals the listener's worker thread to unblock and exit. The actual
 * STOPPED transition is applied later by process_events(). Returns 0. */
int listener_manager_stop(ListenerManager *manager, uint64_t id);

/* Requires object_can_restart() (RUNNING/STOPPED/ERROR); returns -1 and
 * does nothing otherwise. From STOPPED/ERROR, behaves exactly like
 * start_async(). From RUNNING, stops first (like stop()) and
 * automatically starts again once STOPPED is observed via
 * process_events() - no separate call needed. Returns 0. */
int listener_manager_restart(ListenerManager *manager, uint64_t id);

/* Removes the listener from the registry. Not gated by a predicate -
 * removal is always available - but guarded: if RUNNING, stops it first
 * (like stop()) and removes automatically once STOPPED is observed via
 * process_events(), the same deferred-action shape restart() uses.
 * Otherwise removes immediately. Returns -1 if id doesn't exist, 0
 * otherwise (including when the actual removal is still pending). */
int listener_manager_remove(ListenerManager *manager, uint64_t id);

/* Applies the outcome of a start attempt. Requires the listener to
 * currently be STARTING; returns -1 and does nothing otherwise. On
 * success: -> RUNNING, sets runtime.started_at = now, clears last_error,
 * pushes LISTENER_EVENT_STARTED. On failure: -> ERROR, sets
 * runtime.last_error = strdup(error_message), pushes
 * LISTENER_EVENT_START_FAILED carrying error_message. Returns 0. */
int listener_manager_report_start_result(ListenerManager *manager, uint64_t id, bool success,
                                          const char *error_message);

/* Applies the outcome of a stop. Requires STOPPING; returns -1 and does
 * nothing otherwise. -> STOPPED, pushes LISTENER_EVENT_STOPPED.
 * Returns 0. */
int listener_manager_report_stop_result(ListenerManager *manager, uint64_t id);

/* Drains currently-queued events and applies whatever registry
 * transition each implies (STARTED/START_FAILED/STOPPED from a worker's
 * reported outcome, CONNECTION_OPENED by registering a new Connection -
 * its new id is written into the returned event's `sequence` field,
 * since `object_id` stays the *listener's* id for this event type;
 * CREATED/STARTING/STOPPING are no-ops here, already applied
 * synchronously by the calls that pushed them). START_FAILED/STOPPED
 * also join and drop that listener's worker thread. Every drained event
 * is copied into out too, so a caller - a test now, the GUI's own
 * view-state logic from a later phase - can react to the same events
 * this call already consumed (e.g. dispatching CONNECTION_OPENED/
 * CONNECTION_CLOSED to a ConnectionManager), without a second,
 * competing consumer of the queue. If out is NULL, drains the whole
 * queue without copying (max_out is ignored); otherwise copies up to
 * max_out events, leaving any remainder queued for the next call.
 * Returns the number of events processed. */
int listener_manager_process_events(ListenerManager *manager, ListenerEvent *out, int max_out);

#endif /* TOOLBOX_LISTENER_MANAGER_H */
