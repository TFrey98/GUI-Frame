#include "https_worker.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HTTPS_IO_TIMEOUT_SECONDS 3
#define HTTPS_REQUEST_BUF_SIZE 8192

/* Heap-allocated, handed to the worker thread and freed by that same
 * thread right before it returns - same convention as
 * tcp_worker.c/http_worker.c's WorkerArgs. */
typedef struct WorkerArgs {
    EventQueue *events;
    uint64_t listener_id;
    char *bind_address; /* owned; freed by the worker thread */
    uint16_t port;
    char *url_path;    /* owned; freed by the worker thread */
    char *host_header; /* owned; freed by the worker thread; may be empty */
    char *cert_path;   /* owned; freed by the worker thread */
    char *key_path;    /* owned; freed by the worker thread */
    int stop_pipe_read_fd;
} WorkerArgs;

/* Same as tcp_worker.c/http_worker.c's own copies - small enough, and
 * specific enough to each worker's own accept loop, that duplicating
 * them here reads more clearly than sharing a common module. */
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

/* Builds a TLS server context and loads cert_path/key_path into it.
 * Called before bind/listen, so a bad cert/key never even occupies a
 * port - just like tcp_worker.c's own bind() failure, the caller
 * reports this as LISTENER_EVENT_START_FAILED and the listener never
 * reaches RUNNING. Returns NULL and fills err_out on any failure. */
static SSL_CTX *build_server_ctx(const char *cert_path, const char *key_path, char *err_out, size_t err_out_len) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        snprintf(err_out, err_out_len, "failed to create TLS context");
        return NULL;
    }
    if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) != 1) {
        snprintf(err_out, err_out_len, "failed to load certificate: %s", cert_path);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1) {
        snprintf(err_out, err_out_len, "failed to load private key: %s", key_path);
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

/* Bounds subsequent blocking read()/write() syscalls on conn_fd
 * (including the ones SSL_accept()/SSL_read()/SSL_write() make
 * internally) via the kernel's own socket timeout - simpler than a
 * poll()-driven WANT_READ/WANT_WRITE retry loop and just as effective
 * here, since accepted connection fds in this project are never
 * switched to non-blocking mode (see connection_worker.c's own
 * poll-then-blocking-call pattern for the same assumption on the
 * ongoing-I/O side). */
static void set_socket_timeout(int fd, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* SSL-aware twin of http_worker.c's read_http_headers() - same
 * "read until blank line, buffer, or timeout" shape, but the bound
 * comes from set_socket_timeout() above rather than an explicit
 * poll() loop. Returns the number of bytes read (buf is
 * NUL-terminated either way) on success, -1 on timeout/error/overflow. */
static ssize_t read_https_headers(SSL *ssl, char *buf, size_t buf_size) {
    size_t total = 0;
    for (;;) {
        if (total >= buf_size - 1) {
            return -1;
        }
        int n = SSL_read(ssl, buf + total, (int)(buf_size - 1 - total));
        if (n <= 0) {
            return -1; /* timeout, real error, or clean/unclean close */
        }
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n")) {
            return (ssize_t)total;
        }
    }
}

/* Identical parsing logic to http_worker.c's parse_http_request() -
 * duplicated rather than shared, per this file's own header comment. */
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

static void send_all_tls(SSL *ssl, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = SSL_write(ssl, data + sent, (int)(len - sent));
        if (n <= 0) {
            return; /* timeout or error - best-effort response, matches http_worker.c's send_all() */
        }
        sent += (size_t)n;
    }
}

/* Handshakes, reads and matches the request, and either pushes
 * CONNECTION_OPENED with event.tls set (match - ssl/conn_fd ownership
 * passes to the event's consumer, same handoff socket_fd already gets)
 * or tears the connection down itself (no match, or any failure along
 * the way). */
static void handle_https_accept(int conn_fd, SSL_CTX *ctx, const struct sockaddr_storage *peer_addr,
                                 EventQueue *events, uint64_t listener_id, const char *url_path,
                                 const char *host_header) {
    set_socket_timeout(conn_fd, HTTPS_IO_TIMEOUT_SECONDS);

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        close(conn_fd);
        return;
    }
    SSL_set_fd(ssl, conn_fd);

    if (SSL_accept(ssl) != 1) {
        SSL_free(ssl);
        close(conn_fd);
        return;
    }

    char buf[HTTPS_REQUEST_BUF_SIZE];
    if (read_https_headers(ssl, buf, sizeof(buf)) < 0) {
        SSL_free(ssl);
        close(conn_fd);
        return;
    }

    char path[2048];
    char host[256];
    if (!parse_http_request(buf, path, sizeof(path), host, sizeof(host))) {
        SSL_free(ssl);
        close(conn_fd);
        return;
    }

    bool path_matches = strcmp(path, url_path) == 0;
    bool host_matches = !host_header || !*host_header || strcasecmp(host, host_header) == 0;

    if (path_matches && host_matches) {
        static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        send_all_tls(ssl, ok, sizeof(ok) - 1);

        ListenerEvent event = {0};
        event.type = LISTENER_EVENT_CONNECTION_OPENED;
        event.object_id = listener_id;
        event.socket_fd = conn_fd;
        event.tls = ssl;
        format_peer_address(peer_addr, event.remote_host, sizeof(event.remote_host), &event.remote_port);
        event_queue_push(events, event);
        return;
    }

    static const char not_found[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    send_all_tls(ssl, not_found, sizeof(not_found) - 1);
    SSL_free(ssl);
    close(conn_fd);
}

static void *https_worker_main(void *arg) {
    WorkerArgs *args = arg;
    EventQueue *events = args->events;
    uint64_t listener_id = args->listener_id;
    int listen_fd = -1;
    bool started = false;
    SSL_CTX *ctx = NULL;

    char ctx_err[128];
    ctx = build_server_ctx(args->cert_path, args->key_path, ctx_err, sizeof(ctx_err));
    if (!ctx) {
        push_start_failed(events, listener_id, ctx_err);
        goto done;
    }

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
                    continue; /* transient accept() failure - keep accepting, same as tcp_worker's loop */
                }
                handle_https_accept(conn_fd, ctx, &peer_addr, events, listener_id, args->url_path,
                                     args->host_header);
            }
        }
    }

done:
    if (ctx) {
        SSL_CTX_free(ctx);
    }
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
    free(args->cert_path);
    free(args->key_path);
    free(args);
    return NULL;
}

int https_worker_start(TcpWorker *out, EventQueue *events, uint64_t listener_id, const char *bind_address,
                        uint16_t port, const char *url_path, const char *host_header, const char *cert_path,
                        const char *key_path) {
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
    args->cert_path = strdup(cert_path ? cert_path : "");
    args->key_path = strdup(key_path ? key_path : "");
    args->stop_pipe_read_fd = pipe_fds[0];

    pthread_t thread;
    if (pthread_create(&thread, NULL, https_worker_main, args) != 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        free(args->bind_address);
        free(args->url_path);
        free(args->host_header);
        free(args->cert_path);
        free(args->key_path);
        free(args);
        return -1;
    }

    out->thread = thread;
    out->stop_pipe_write_fd = pipe_fds[1];
    return 0;
}
