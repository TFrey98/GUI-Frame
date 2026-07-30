#ifndef TOOLBOX_HTTPS_WORKER_H
#define TOOLBOX_HTTPS_WORKER_H

#include "event_queue.h"
#include "tcp_worker.h"

/*
 * Same manager interface, new backend, one layer deeper than
 * http_worker.h: an HTTPS listener's worker binds/listens/accepts
 * exactly like tcp_worker.h/http_worker.h, reusing TcpWorker's struct
 * and tcp_worker_signal_stop()/tcp_worker_join() unchanged. Before
 * bind/listen, it builds an SSL_CTX and loads cert_path/key_path - a
 * failure there is reported as LISTENER_EVENT_START_FAILED exactly like
 * a bad bind_address is, giving "bad cert path -> clean START_FAILED"
 * for free from the same machinery. Per accepted connection: a TLS
 * server handshake, then the same bounded request-line/header read and
 * url_path/host_header matching http_worker.c does (duplicated, not
 * shared - see that file's own header comment on why). A match gets a
 * 200 over TLS and LISTENER_EVENT_CONNECTION_OPENED with event.tls set
 * to the handshake-completed SSL*, handed off exactly like an HTTP or
 * reverse-TCP accept from that point on; a non-match (or a failed
 * handshake) gets the connection torn down (SSL_free + close) without
 * ever becoming a Connection.
 */

/* url_path, host_header, cert_path, and key_path are copied (the
 * caller's own copies may be freed independently once this returns) -
 * host_header may be NULL or empty to mean "don't check the Host
 * header." Same return/event contract as tcp_worker_start(). */
int https_worker_start(TcpWorker *out, EventQueue *events, uint64_t listener_id, const char *bind_address,
                        uint16_t port, const char *url_path, const char *host_header, const char *cert_path,
                        const char *key_path);

#endif /* TOOLBOX_HTTPS_WORKER_H */
