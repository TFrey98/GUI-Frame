#include "http_worker.h"

#include <arpa/inet.h>
#include <ctype.h>
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

#define HTTP_HANDSHAKE_TIMEOUT_MS 3000
#define HTTP_REQUEST_BUF_SIZE 8192

/* Heap-allocated, handed to the worker thread and freed by that same
 * thread right before it returns - same convention as tcp_worker.c's
 * WorkerArgs. */
typedef struct WorkerArgs {
    EventQueue *events;
    uint64_t listener_id;
    char *bind_address; /* owned; freed by the worker thread */
    uint16_t port;
    char *url_path;    /* owned; freed by the worker thread */
    char *host_header; /* owned; freed by the worker thread; may be empty */
    int stop_pipe_read_fd;
} WorkerArgs;

/* Same as tcp_worker.c's build_bind_address()/format_peer_address() -
 * small enough, and specific enough to each worker's own accept loop,
 * that duplicating them here reads more clearly than sharing a common
 * module for four short helpers. */
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

/* Reads until the blank line ending HTTP headers, a fixed timeout, or
 * running out of buffer - whichever comes first. Returns the number of
 * bytes read (buf is NUL-terminated either way) on success, -1 on
 * timeout/error/overflow. Bytes past the blank line, if any arrived in
 * the same read, are included but never inspected - see the "scope
 * trim" note in http_worker.h. */
static ssize_t read_http_headers(int fd, char *buf, size_t buf_size) {
    size_t total = 0;
    for (;;) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int ready = poll(&pfd, 1, HTTP_HANDSHAKE_TIMEOUT_MS);
        if (ready <= 0) {
            return -1;
        }
        if (total >= buf_size - 1) {
            return -1;
        }
        ssize_t n = read(fd, buf + total, buf_size - 1 - total);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n")) {
            return (ssize_t)total;
        }
    }
}

/* Minimal, purpose-built parser - not a general HTTP implementation.
 * Pulls the request line's path ("<METHOD> <PATH> HTTP/x.x") and the
 * Host header's value, if present. Returns false only if the request
 * line itself couldn't be parsed at all (Host is optional). */
static bool parse_http_request(const char *buf, char *path_out, size_t path_out_len, char *host_out,
                                size_t host_out_len) {
    const char *sp1 = strchr(buf, ' ');
    if (!sp1) {
        return false;
    }
    const char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) {
        return false;
    }
    size_t path_len = (size_t)(sp2 - (sp1 + 1));
    if (path_len == 0 || path_len >= path_out_len) {
        return false;
    }
    memcpy(path_out, sp1 + 1, path_len);
    path_out[path_len] = '\0';

    host_out[0] = '\0';
    /* Scans header lines one at a time for one that case-insensitively
     * starts with "Host:" - avoids strcasestr(), a GNU extension that
     * isn't reliably declared under every build configuration (it
     * silently fell back to an implicit int-returning declaration here,
     * truncating the returned pointer and segfaulting; strncasecmp is
     * POSIX and doesn't have that problem). */
    const char *line = strstr(buf, "\r\n");
    if (!line) {
        line = strchr(buf, '\n');
    }
    while (line && *line) {
        while (*line == '\r' || *line == '\n') {
            line++;
        }
        if (*line == '\0') {
            break;
        }
        if (strncasecmp(line, "Host:", 5) == 0) {
            const char *value = line + 5;
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            const char *end = strstr(value, "\r\n");
            if (!end) {
                end = strchr(value, '\n');
            }
            if (!end) {
                end = value + strlen(value);
            }
            size_t len = (size_t)(end - value);
            if (len < host_out_len) {
                memcpy(host_out, value, len);
                host_out[len] = '\0';
            }
            break;
        }
        line = strstr(line, "\r\n");
        if (!line) {
            break;
        }
    }
    return true;
}

static void send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n <= 0) {
            return;
        }
        sent += (size_t)n;
    }
}

/* Returns true (and sends a 200) if this connection's request matches
 * url_path/host_header and should become a real Connection; false
 * (having already sent a 404 and left the socket ready to close) if
 * it should be rejected. */
static bool handle_http_handshake(int conn_fd, const char *url_path, const char *host_header) {
    char buf[HTTP_REQUEST_BUF_SIZE];
    if (read_http_headers(conn_fd, buf, sizeof(buf)) < 0) {
        return false;
    }

    char path[2048];
    char host[256];
    if (!parse_http_request(buf, path, sizeof(path), host, sizeof(host))) {
        return false;
    }

    bool path_matches = strcmp(path, url_path) == 0;
    bool host_matches = !host_header || !*host_header || strcasecmp(host, host_header) == 0;

    if (path_matches && host_matches) {
        static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        send_all(conn_fd, ok, sizeof(ok) - 1);
        return true;
    }

    static const char not_found[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    send_all(conn_fd, not_found, sizeof(not_found) - 1);
    return false;
}

static void *http_worker_main(void *arg) {
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
                break;
            }
            if (fds[1].revents & POLLIN) {
                break; /* stop requested */
            }
            if (fds[0].revents & POLLIN) {
                struct sockaddr_storage peer_addr;
                socklen_t peer_len = sizeof(peer_addr);
                int conn_fd = accept(listen_fd, (struct sockaddr *)&peer_addr, &peer_len);
                if (conn_fd < 0) {
                    /* Transient accept() failure (e.g. EMFILE) - keep
                     * accepting subsequent connections, same as
                     * tcp_worker's loop. */
                    continue;
                }

                if (handle_http_handshake(conn_fd, args->url_path, args->host_header)) {
                    ListenerEvent event = {0};
                    event.type = LISTENER_EVENT_CONNECTION_OPENED;
                    event.object_id = listener_id;
                    event.socket_fd = conn_fd;
                    format_peer_address(&peer_addr, event.remote_host, sizeof(event.remote_host),
                                         &event.remote_port);
                    event_queue_push(events, event);
                } else {
                    close(conn_fd);
                }
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
    free(args->url_path);
    free(args->host_header);
    free(args);
    return NULL;
}

int http_worker_start(TcpWorker *out, EventQueue *events, uint64_t listener_id, const char *bind_address,
                       uint16_t port, const char *url_path, const char *host_header) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        return -1;
    }

    WorkerArgs *args = malloc(sizeof(WorkerArgs));
    args->events = events;
    args->listener_id = listener_id;
    args->bind_address = strdup(bind_address);
    args->port = port;
    args->url_path = strdup(url_path ? url_path : "");
    args->host_header = strdup(host_header ? host_header : "");
    args->stop_pipe_read_fd = pipe_fds[0];

    pthread_t thread;
    if (pthread_create(&thread, NULL, http_worker_main, args) != 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        free(args->bind_address);
        free(args->url_path);
        free(args->host_header);
        free(args);
        return -1;
    }

    out->thread = thread;
    out->stop_pipe_write_fd = pipe_fds[1];
    return 0;
}
