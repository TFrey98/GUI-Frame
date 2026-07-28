#ifndef TOOLBOX_CONNECTION_WORKER_H
#define TOOLBOX_CONNECTION_WORKER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "byte_buffer.h"
#include "event_queue.h"

/*
 * One reader/writer thread per CONNECTED connection, same self-pipe
 * shape as tcp_worker.h. Reads into `incoming` (GUI thread drains it
 * into TerminalHistory each tick), writes whatever's in `outgoing` (GUI
 * thread appends to it via connection_manager_send()). Never touches
 * ObjectRegistry directly - reports everything back via EventQueue,
 * same ownership model as every other worker in this project.
 */
typedef struct ConnectionWorker {
    pthread_t thread;
    int wake_pipe_write_fd; /* one pipe, dual purpose: new-outgoing-data wake, or stop wake */
    ByteBuffer *outgoing;
    ByteBuffer *incoming;
    atomic_bool *stop_requested; /* heap-allocated, shared with the thread; freed by connection_worker_join */
} ConnectionWorker;

/* Spawns the worker thread immediately, taking ownership of socket_fd
 * (the worker closes it on exit). Pushes LISTENER_EVENT_CONNECTION_CLOSED
 * for connection_id once the connection ends, for any reason (remote
 * EOF, a local error, or being told to stop).
 *
 * Returns 0 on success (out is filled in), -1 if the thread/pipe
 * couldn't even be created (out is left untouched - the caller should
 * synthesize its own CONNECTION_CLOSED, since no thread exists to
 * report one; socket_fd is NOT closed in this failure case, since
 * ownership was never actually transferred). */
int connection_worker_start(ConnectionWorker *out, EventQueue *events, uint64_t connection_id, int socket_fd);

/* Wakes the worker's poll() without requesting a stop - call after
 * appending new data to worker->outgoing. */
void connection_worker_notify_outgoing(ConnectionWorker *worker);

/* Requests a stop and wakes the worker's poll() to notice it. Does not
 * block or join - the worker finishes asynchronously and reports back
 * via LISTENER_EVENT_CONNECTION_CLOSED. */
void connection_worker_signal_stop(ConnectionWorker *worker);

/* Joins the thread, then frees its outgoing/incoming buffers, the
 * shared stop flag, and the wake pipe's write end. Drain
 * worker->incoming yourself first if you still want its contents - this
 * destroys it. Only safe to call once the worker's
 * LISTENER_EVENT_CONNECTION_CLOSED has already been observed. */
void connection_worker_join(ConnectionWorker *worker);

#endif /* TOOLBOX_CONNECTION_WORKER_H */
