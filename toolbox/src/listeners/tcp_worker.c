#include "tcp_worker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Heap-allocated, handed to the worker thread and freed by that same
 * thread right before it returns - the spawning side never touches it
 * again once pthread_create succeeds. */
typedef struct WorkerArgs {
    EventQueue *events;
    uint64_t listener_id;
    char *bind_address; /* owned; freed by the worker thread */
    uint16_t port;
    int stop_pipe_read_fd;
} WorkerArgs;

/* Tries bind_address as IPv4 then IPv6 (same order as
 * listener_config_validate), filling storage/addr_len on success.
 * Returns the resolved address family, or -1 if bind_address is neither.
 * Listener validation normally catches that first, but the worker remains
 * defensive because it is an independent subsystem boundary. */
static int build_bind_address(const char *bind_address, uint16_t port, struct sockaddr_storage *storage,
                               socklen_t *addr_len) {
    memset(storage, 0, sizeof(*storage));

    struct sockaddr_in addr4 = {0};
    if (inet_pton(AF_INET, bind_address, &addr4.sin_addr) == 1) {
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(port);
        memcpy(storage, &addr4, sizeof(addr4));
        *addr_len = sizeof(addr4);
        return AF_INET;
    }

    struct sockaddr_in6 addr6 = {0};
    if (inet_pton(AF_INET6, bind_address, &addr6.sin6_addr) == 1) {
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);
        memcpy(storage, &addr6, sizeof(addr6));
        *addr_len = sizeof(addr6);
        return AF_INET6;
    }

    return -1;
}

static void format_peer_address(const struct sockaddr_storage *addr, char *host_out, size_t host_out_len,
                                 uint16_t *port_out) {
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *addr4 = (const struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &addr4->sin_addr, host_out, host_out_len);
        *port_out = ntohs(addr4->sin_port);
    } else if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *addr6 = (const struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &addr6->sin6_addr, host_out, host_out_len);
        *port_out = ntohs(addr6->sin6_port);
    } else if (host_out_len > 0) {
        host_out[0] = '\0';
        *port_out = 0;
    }
}

static void push_start_failed(EventQueue *events, uint64_t listener_id, const char *message) {
    ListenerEvent event = {0};
    event.type = LISTENER_EVENT_START_FAILED;
    event.object_id = listener_id;
    snprintf(event.message, sizeof(event.message), "%s", message);
    event_queue_push(events, event);
}

/* Formats errno via the XSI-compliant strerror_r (int return, fills
 * errbuf) - the variant this project's headers actually resolve to;
 * thread-safe, unlike plain strerror(), which matters here since
 * multiple worker threads can fail concurrently. */
static void push_start_failed_errno(EventQueue *events, uint64_t listener_id, const char *context) {
    char errbuf[100];
    int rc = strerror_r(errno, errbuf, sizeof(errbuf));
    const char *detail = (rc == 0) ? errbuf : "unknown error";
    ListenerEvent event = {0};
    event.type = LISTENER_EVENT_START_FAILED;
    event.object_id = listener_id;
    snprintf(event.message, sizeof(event.message), "%s: %s", context, detail);
    event_queue_push(events, event);
}

static void *tcp_worker_main(void *arg) {
    WorkerArgs *args = arg;
    EventQueue *events = args->events;
    uint64_t listener_id = args->listener_id;
    int listen_fd = -1;
    bool started = false;

    struct sockaddr_storage addr;
    socklen_t addr_len;
    int family = build_bind_address(args->bind_address, args->port, &addr, &addr_len);
    if (family < 0) {
        push_start_failed(events, listener_id, "invalid bind address");
        goto done;
    }

    listen_fd = socket(family, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        push_start_failed_errno(events, listener_id, "socket");
        goto done;
    }

    {
        int reuse = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    }

    if (bind(listen_fd, (struct sockaddr *)&addr, addr_len) != 0) {
        push_start_failed_errno(events, listener_id, "bind");
        goto done;
    }

    if (listen(listen_fd, 16) != 0) {
        push_start_failed_errno(events, listener_id, "listen");
        goto done;
    }

    started = true;
    {
        ListenerEvent event = {0};
        event.type = LISTENER_EVENT_STARTED;
        event.object_id = listener_id;
        event.socket_fd = listen_fd;
        event.started_at = time(NULL);
        event_queue_push(events, event);
    }

    {
        struct pollfd fds[2];
        fds[0].fd = listen_fd;
        fds[0].events = POLLIN;
        fds[1].fd = args->stop_pipe_read_fd;
        fds[1].events = POLLIN;

        for (;;) {
            int ready = poll(fds, 2, -1);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break; /* unexpected poll() failure - fall through and report stopped */
            }
            if (fds[1].revents & POLLIN) {
                break; /* stop requested */
            }
            if (fds[0].revents & POLLIN) {
                struct sockaddr_storage peer_addr;
                socklen_t peer_len = sizeof(peer_addr);
                int conn_fd = accept(listen_fd, (struct sockaddr *)&peer_addr, &peer_len);
                if (conn_fd >= 0) {
                    ListenerEvent event = {0};
                    event.type = LISTENER_EVENT_CONNECTION_OPENED;
                    event.object_id = listener_id;
                    event.socket_fd = conn_fd;
                    format_peer_address(&peer_addr, event.remote_host, sizeof(event.remote_host),
                                         &event.remote_port);
                    event_queue_push(events, event);
                }
                /* accept() failing here (e.g. a transient EMFILE) is
                 * just skipped - the loop keeps accepting subsequent
                 * connections rather than tearing down the listener. */
            }
        }
    }

done:
    if (listen_fd >= 0) {
        close(listen_fd);
    }
    close(args->stop_pipe_read_fd);

    if (started) {
        ListenerEvent event = {0};
        event.type = LISTENER_EVENT_STOPPED;
        event.object_id = listener_id;
        event_queue_push(events, event);
    }

    free(args->bind_address);
    free(args);
    return NULL;
}

int tcp_worker_start(TcpWorker *out, EventQueue *events, uint64_t listener_id, const char *bind_address,
                      uint16_t port) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        return -1;
    }

    WorkerArgs *args = malloc(sizeof(WorkerArgs));
    args->events = events;
    args->listener_id = listener_id;
    args->bind_address = strdup(bind_address);
    args->port = port;
    args->stop_pipe_read_fd = pipe_fds[0];

    pthread_t thread;
    if (pthread_create(&thread, NULL, tcp_worker_main, args) != 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        free(args->bind_address);
        free(args);
        return -1;
    }

    out->thread = thread;
    out->stop_pipe_write_fd = pipe_fds[1];
    return 0;
}

void tcp_worker_signal_stop(TcpWorker *worker) {
    char byte = 1;
    ssize_t ignored = write(worker->stop_pipe_write_fd, &byte, 1);
    (void)ignored;
}

void tcp_worker_join(TcpWorker *worker) {
    pthread_join(worker->thread, NULL);
    close(worker->stop_pipe_write_fd);
}
