#ifndef WORKBENCH_LISTENER_H
#define WORKBENCH_LISTENER_H

#include <stdint.h>
#include <time.h>

/*
 * Plain data only - construction and destruction belong to
 * ObjectRegistry, the sole owner of every Listener (see object_registry.h).
 */

typedef enum ListenerType {
    LISTENER_TYPE_REVERSE_TCP,
    LISTENER_TYPE_HTTP,  /* unused until the HTTP provider phase */
    LISTENER_TYPE_HTTPS, /* unused until the HTTPS/TLS provider phase */
} ListenerType;

typedef enum ListenerState {
    LISTENER_STATE_CONFIGURED, /* created, never started */
    LISTENER_STATE_STARTING,
    LISTENER_STATE_RUNNING,
    LISTENER_STATE_STOPPING,
    LISTENER_STATE_STOPPED, /* cleanly stopped, can be started again */
    LISTENER_STATE_ERROR,   /* start failed or the listener crashed */
} ListenerState;

typedef struct ListenerConfig {
    char *name;
    ListenerType type;
    char *bind_address;  /* interface to listen on */
    uint16_t port;
    char *callback_host; /* independent of bind_address - what's shown
                           * to the operator as the address a payload
                           * should call back to; never 0.0.0.0/:: */
    char *cert_path;      /* required when type == LISTENER_TYPE_HTTPS */
    char *key_path;       /* required when type == LISTENER_TYPE_HTTPS */
    char *url_path;       /* required when type == LISTENER_TYPE_HTTP(S) - an inbound
                            * request's path must match exactly before it becomes a
                            * Connection */
    char *host_header;    /* optional when type == LISTENER_TYPE_HTTP(S) - if set, an
                            * inbound request's Host header must also match exactly */
} ListenerConfig;

typedef struct ListenerRuntime {
    ListenerState state;
    time_t started_at; /* 0 if never started */
    char *last_error;  /* NULL unless state == LISTENER_STATE_ERROR */
} ListenerRuntime;

typedef struct Listener {
    uint64_t id;
    ListenerConfig config;
    ListenerRuntime runtime;
} Listener;

#endif /* WORKBENCH_LISTENER_H */
