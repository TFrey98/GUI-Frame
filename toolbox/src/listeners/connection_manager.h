#ifndef TOOLBOX_CONNECTION_MANAGER_H
#define TOOLBOX_CONNECTION_MANAGER_H

#include "event_queue.h"
#include "object_registry.h"

/*
 * Applies what a Connection's worker thread (connection_worker.h)
 * reports, the same layering ListenerManager established for listener
 * workers. Deliberately doesn't drain EventQueue itself - see the
 * Phase 4 plan for why listener_manager_process_events() stays the
 * sole drain loop; the caller inspects events it returns and dispatches
 * CONNECTION_OPENED/CONNECTION_CLOSED here.
 */
typedef struct ConnectionManager ConnectionManager;

/* registry and events are borrowed, not owned - both must outlive the
 * manager. */
ConnectionManager *connection_manager_create(ObjectRegistry *registry, EventQueue *events);
void connection_manager_destroy(ConnectionManager *manager);

/* Call after seeing a CONNECTION_OPENED event returned from
 * listener_manager_process_events() - by then the Connection already
 * exists in the registry (created as a side effect of that same call).
 * Looks up its socket_fd and spawns its worker thread. If the worker
 * itself fails to start, marks the connection DISCONNECTED immediately
 * and pushes its own CONNECTION_CLOSED (mirrors how listener_manager's
 * start_async handles tcp_worker_start failing). Returns -1 if no such
 * connection exists, 0 otherwise. */
int connection_manager_start_worker(ConnectionManager *manager, uint64_t connection_id);

/* Call after seeing a CONNECTION_CLOSED event (from a worker noticing
 * EOF, one told to stop via disconnect() below, or start_worker's own
 * failure path above). Drains any last unread bytes into the
 * connection's TerminalHistory, joins/reaps its worker if one exists,
 * and marks it DISCONNECTED - object and history retained, never
 * removed. Safe to call even with no worker registered (idempotent). */
void connection_manager_handle_closed(ConnectionManager *manager, uint64_t connection_id);

/* Requires object_can_stop() (== CONNECTED); returns -1 and does
 * nothing otherwise. Only signals the worker to stop - does NOT
 * transition state itself. That happens later, when the caller sees
 * this connection's CONNECTION_CLOSED and calls
 * connection_manager_handle_closed() - the exact same path a
 * remote-initiated disconnect takes, so both converge on identical
 * behavior. Returns 0. */
int connection_manager_disconnect(ConnectionManager *manager, uint64_t connection_id);

/* Removes the connection from the registry. Not gated by a predicate -
 * removal is always available - but guarded: if CONNECTED, disconnects
 * first and removes automatically once connection_manager_handle_closed()
 * has actually joined the worker (the same safe point a real disconnect
 * uses), since the worker thread is still concurrently touching
 * Connection.socket_fd until then. Otherwise removes immediately.
 * Returns -1 if connection_id doesn't exist, 0 otherwise (including when
 * the actual removal is still pending). */
int connection_manager_remove(ConnectionManager *manager, uint64_t connection_id);

/* Appends data to the connection's outgoing buffer and wakes its worker
 * to flush it. Returns -1 if no live worker for connection_id, 0
 * otherwise. No single-writer enforcement here - the doc defers that to
 * the phase that introduces TerminalView attach/detach. */
int connection_manager_send(ConnectionManager *manager, uint64_t connection_id, const void *data, size_t len);

/* Call once per tick (alongside listener_manager_process_events,
 * regardless of whether any event fired) - drains every live
 * connection's incoming buffer into its TerminalHistory. */
void connection_manager_drain_data(ConnectionManager *manager);

#endif /* TOOLBOX_CONNECTION_MANAGER_H */
