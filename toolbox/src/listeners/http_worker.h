#ifndef TOOLBOX_HTTP_WORKER_H
#define TOOLBOX_HTTP_WORKER_H

#include "event_queue.h"
#include "tcp_worker.h"

/*
 * Same manager interface, new backend: an HTTP listener's worker binds/
 * listens/accepts exactly like tcp_worker.h, and its thread+stop-pipe
 * lifecycle is genuinely identical - it reuses TcpWorker's struct and
 * tcp_worker_signal_stop()/tcp_worker_join() unchanged rather than
 * duplicating them. The only real difference is per accepted
 * connection: a bounded blocking read of the request line and headers,
 * checked against url_path (required) and host_header (optional - a
 * blank host_header skips that check). A match gets a 200 response and
 * LISTENER_EVENT_CONNECTION_OPENED, handed off exactly like a
 * reverse-TCP accept from that point on; a non-match gets a 404 and the
 * socket is closed without ever becoming a Connection.
 *
 * Scope trim: any bytes after the header/body boundary in the same read
 * (e.g. a POST body, or traffic pipelined right after headers) are
 * consumed by the handshake parser and lost - there's no mechanism to
 * hand them to the Connection's own read loop afterward. Bodyless GET
 * requests never hit this.
 */

/* url_path and host_header are copied (the caller's own copies may be
 * freed independently once this returns) - host_header may be NULL or
 * empty to mean "don't check the Host header." Same return/event
 * contract as tcp_worker_start(). */
int http_worker_start(TcpWorker *out, EventQueue *events, uint64_t listener_id, const char *bind_address,
                       uint16_t port, const char *url_path, const char *host_header);

#endif /* TOOLBOX_HTTP_WORKER_H */
