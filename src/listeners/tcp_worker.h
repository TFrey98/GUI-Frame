#ifndef WORKBENCH_TCP_WORKER_H
#define WORKBENCH_TCP_WORKER_H

#include <pthread.h>
#include <stdint.h>

#include "event_queue.h"

/*
 * Owns one reverse-TCP listening socket's entire lifetime on its own
 * thread: binds+listens, then accepts connections until told to stop,
 * reporting everything back as ListenerEvents for ListenerManager to
 * apply on the GUI thread. Never touches ObjectRegistry directly - see
 * the top-level ownership model (worker threads only push events).
 */
typedef struct TcpWorker {
    pthread_t thread;
    int stop_pipe_write_fd;
} TcpWorker;

/* Spawns the worker thread immediately; bind_address is copied (the
 * caller's own copy may be freed independently once this returns).
 * Pushes LISTENER_EVENT_STARTED or LISTENER_EVENT_START_FAILED for
 * listener_id once the bind/listen attempt resolves, then (on success)
 * LISTENER_EVENT_CONNECTION_OPENED per accepted connection, then
 * LISTENER_EVENT_STOPPED once told to stop.
 *
 * Returns 0 on success (out is filled in), -1 if the thread or its stop
 * pipe could not even be created (out is left untouched - the caller
 * should synthesize its own START_FAILED, since no thread exists to
 * report one). */
int tcp_worker_start(TcpWorker *out, EventQueue *events, uint64_t listener_id, const char *bind_address,
                      uint16_t port);

/* Writes to the stop pipe, unblocking the worker's poll(). Does not
 * block or join - the worker finishes asynchronously and reports back
 * via LISTENER_EVENT_STOPPED. */
void tcp_worker_signal_stop(TcpWorker *worker);

/* Joins the worker's thread. Only safe to call once the worker's
 * terminal event (START_FAILED or STOPPED) has already been observed -
 * by that point the thread has returned or is about to, so this
 * returns quickly. */
void tcp_worker_join(TcpWorker *worker);

#endif /* WORKBENCH_TCP_WORKER_H */
