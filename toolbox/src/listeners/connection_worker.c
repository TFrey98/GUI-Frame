#include "connection_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#define CONNECTION_WORKER_CHUNK_SIZE 4096

/* Heap-allocated, handed to the worker thread and freed by that same
 * thread right before it returns. outgoing/incoming/stop_requested are
 * borrowed - owned by the caller's ConnectionWorker (which outlives
 * this thread), not by this struct. tls, like socket_fd, IS owned by
 * this struct/thread - see connection_worker.h's comment. */
typedef struct WorkerArgs {
    EventQueue *events;
    uint64_t connection_id;
    int socket_fd;
    void *tls; /* NULL for a plain connection; an SSL* for an HTTPS one */
    int wake_pipe_read_fd;
    ByteBuffer *outgoing;
    ByteBuffer *incoming;
    atomic_bool *stop_requested;
} WorkerArgs;

/* Unifies the plain-socket EAGAIN/EWOULDBLOCK case with TLS's
 * SSL_ERROR_WANT_READ/WANT_WRITE - both mean "not ready yet, not an
 * error, not EOF, just try again once poll() says so." socket_fd is
 * blocking (never switched to O_NONBLOCK anywhere in this project), so
 * a plain read() only reaches this function after poll() already said
 * POLLIN, meaning it won't actually block; WANT_READ/WANT_WRITE on the
 * TLS side is likewise expected to be rare on a blocking fd but is
 * still handled correctly rather than assumed away, since a TLS
 * record can legitimately need more bytes than happened to arrive in
 * one read. Returns bytes read (>0) on success; 0 with *would_block
 * left false on real EOF/error (caller should treat the connection as
 * done); 0 with *would_block set to true if the call should just be
 * retried next time this fd is ready. */
static ssize_t socket_read(int fd, void *tls, void *buf, size_t len, bool *would_block) {
    *would_block = false;
    if (tls) {
        SSL *ssl = tls;
        int n = SSL_read(ssl, buf, (int)len);
        if (n > 0) {
            return n;
        }
        int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            *would_block = true;
        }
        return 0;
    }
    ssize_t n = read(fd, buf, len);
    if (n > 0) {
        return n;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        *would_block = true;
    }
    return 0;
}

/* TLS/plain-socket twin of socket_read() for the write path. Returns
 * bytes written (>0) on success; 0 with *would_block set to true if
 * the call should be retried next time this fd is write-ready; -1 on a
 * real error. */
static ssize_t socket_write(int fd, void *tls, const void *buf, size_t len, bool *would_block) {
    *would_block = false;
    if (tls) {
        SSL *ssl = tls;
        int n = SSL_write(ssl, buf, (int)len);
        if (n > 0) {
            return n;
        }
        int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            *would_block = true;
            return 0;
        }
        return -1;
    }
    ssize_t n = write(fd, buf, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *would_block = true;
            return 0;
        }
        return -1;
    }
    return n;
}

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
            bool would_block = false;
            ssize_t n = socket_read(args->socket_fd, args->tls, read_buf, sizeof(read_buf), &would_block);
            if (n > 0) {
                byte_buffer_append(args->incoming, read_buf, (size_t)n);
            } else if (!would_block) {
                break; /* real EOF or error - either way, the connection is done */
            }
        }

        if (fds[0].revents & POLLOUT) {
            /* Peek (not consume) - only discard what write() actually
             * confirmed sending, so a partial write never silently
             * drops the un-sent remainder. */
            size_t n = byte_buffer_peek(args->outgoing, write_buf, sizeof(write_buf));
            if (n > 0) {
                bool would_block = false;
                ssize_t written = socket_write(args->socket_fd, args->tls, write_buf, n, &would_block);
                if (written < 0) {
                    break; /* real error */
                } else if (written > 0) {
                    byte_buffer_discard(args->outgoing, (size_t)written);
                }
            }
        }
    }

    if (args->tls) {
        SSL_free((SSL *)args->tls);
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

int connection_worker_start(ConnectionWorker *out, EventQueue *events, uint64_t connection_id, int socket_fd,
                             void *tls) {
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
    args->tls = tls;
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
