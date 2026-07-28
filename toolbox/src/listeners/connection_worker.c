#include "connection_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#define CONNECTION_WORKER_CHUNK_SIZE 4096

/* Heap-allocated, handed to the worker thread and freed by that same
 * thread right before it returns. outgoing/incoming/stop_requested are
 * borrowed - owned by the caller's ConnectionWorker (which outlives
 * this thread), not by this struct. */
typedef struct WorkerArgs {
    EventQueue *events;
    uint64_t connection_id;
    int socket_fd;
    int wake_pipe_read_fd;
    ByteBuffer *outgoing;
    ByteBuffer *incoming;
    atomic_bool *stop_requested;
} WorkerArgs;

static void *connection_worker_main(void *arg) {
    WorkerArgs *args = arg;

    unsigned char read_buf[CONNECTION_WORKER_CHUNK_SIZE];
    unsigned char write_buf[CONNECTION_WORKER_CHUNK_SIZE];

    for (;;) {
        struct pollfd fds[2];
        fds[0].fd = args->socket_fd;
        /* POLLOUT only when there's actually something queued to send -
         * a healthy socket is almost always write-ready, so including
         * it unconditionally would busy-loop poll() for nothing. The
         * wake pipe is what re-evaluates this the moment new outgoing
         * data shows up while blocked without POLLOUT set. */
        fds[0].events = (short)(POLLIN | (byte_buffer_len(args->outgoing) > 0 ? POLLOUT : 0));
        fds[1].fd = args->wake_pipe_read_fd;
        fds[1].events = POLLIN;

        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; /* unexpected poll() failure - fall through and report closed */
        }

        if (fds[1].revents & POLLIN) {
            /* wake_pipe_read_fd is non-blocking (set in
             * connection_worker_start) specifically so this drain loop
             * terminates via EAGAIN once the pipe is empty, instead of
             * its final read() blocking forever waiting for a byte that
             * isn't coming. */
            char discard[16];
            while (read(args->wake_pipe_read_fd, discard, sizeof(discard)) > 0) {
                /* drain every queued wake byte so poll() doesn't immediately refire */
            }
            if (atomic_load(args->stop_requested)) {
                break;
            }
            /* else: just a "new outgoing data" wake - loop back and recompute pollfds */
        }

        if (fds[0].revents & POLLIN) {
            ssize_t n = read(args->socket_fd, read_buf, sizeof(read_buf));
            if (n > 0) {
                byte_buffer_append(args->incoming, read_buf, (size_t)n);
            } else {
                break; /* n == 0: EOF; n < 0: error - either way, the connection is done */
            }
        }

        if (fds[0].revents & POLLOUT) {
            /* Peek (not consume) - only discard what write() actually
             * confirmed sending, so a partial write never silently
             * drops the un-sent remainder. */
            size_t n = byte_buffer_peek(args->outgoing, write_buf, sizeof(write_buf));
            if (n > 0) {
                ssize_t written = write(args->socket_fd, write_buf, n);
                if (written < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        break;
                    }
                } else if (written > 0) {
                    byte_buffer_discard(args->outgoing, (size_t)written);
                }
            }
        }
    }

    close(args->socket_fd);
    close(args->wake_pipe_read_fd);

    ListenerEvent event = {0};
    event.type = LISTENER_EVENT_CONNECTION_CLOSED;
    event.object_id = args->connection_id;
    event_queue_push(args->events, event);

    free(args);
    return NULL;
}

int connection_worker_start(ConnectionWorker *out, EventQueue *events, uint64_t connection_id, int socket_fd) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        return -1;
    }
    /* Non-blocking read end: the wake path (below) drains every queued
     * wake byte in a loop, and that loop needs read() to return EAGAIN
     * once the pipe is empty rather than blocking on a byte that isn't
     * coming - a blocking fd here would hang the worker forever on its
     * second wake. */
    fcntl(pipe_fds[0], F_SETFL, fcntl(pipe_fds[0], F_GETFL, 0) | O_NONBLOCK);

    atomic_bool *stop_requested = malloc(sizeof(atomic_bool));
    atomic_init(stop_requested, false);
    ByteBuffer *outgoing = byte_buffer_create(0);
    ByteBuffer *incoming = byte_buffer_create(0);

    WorkerArgs *args = malloc(sizeof(WorkerArgs));
    args->events = events;
    args->connection_id = connection_id;
    args->socket_fd = socket_fd;
    args->wake_pipe_read_fd = pipe_fds[0];
    args->outgoing = outgoing;
    args->incoming = incoming;
    args->stop_requested = stop_requested;

    pthread_t thread;
    if (pthread_create(&thread, NULL, connection_worker_main, args) != 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        byte_buffer_destroy(outgoing);
        byte_buffer_destroy(incoming);
        free(stop_requested);
        free(args);
        return -1;
    }

    out->thread = thread;
    out->wake_pipe_write_fd = pipe_fds[1];
    out->outgoing = outgoing;
    out->incoming = incoming;
    out->stop_requested = stop_requested;
    return 0;
}

void connection_worker_notify_outgoing(ConnectionWorker *worker) {
    char byte = 1;
    ssize_t ignored = write(worker->wake_pipe_write_fd, &byte, 1);
    (void)ignored;
}

void connection_worker_signal_stop(ConnectionWorker *worker) {
    atomic_store(worker->stop_requested, true);
    connection_worker_notify_outgoing(worker); /* same wake mechanism, different reason */
}

void connection_worker_join(ConnectionWorker *worker) {
    pthread_join(worker->thread, NULL);
    close(worker->wake_pipe_write_fd);
    byte_buffer_destroy(worker->outgoing);
    byte_buffer_destroy(worker->incoming);
    free(worker->stop_requested);
}
