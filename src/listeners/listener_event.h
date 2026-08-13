#ifndef WORKBENCH_LISTENER_EVENT_H
#define WORKBENCH_LISTENER_EVENT_H

#include <stdint.h>
#include <time.h>

/* Worker-to-main-thread messages. Events are copied by value through
 * EventQueue, so fields must remain self-contained except for explicitly
 * transferred resources such as the CONNECTION_OPENED TLS handle. */
typedef enum ListenerEventType {
    LISTENER_EVENT_TEST, /* synthetic payload used by event_queue_stress_test */
    LISTENER_EVENT_CREATED,
    LISTENER_EVENT_STARTING,
    LISTENER_EVENT_STARTED,
    LISTENER_EVENT_START_FAILED,
    LISTENER_EVENT_STOPPING,
    LISTENER_EVENT_STOPPED,
    LISTENER_EVENT_CONNECTION_OPENED,
    LISTENER_EVENT_CONNECTION_CLOSED,
} ListenerEventType;

typedef struct ListenerEvent {
    ListenerEventType type;
    uint64_t object_id; /* the listener id this event is about (CONNECTION_OPENED: the owning listener's id) */
    uint64_t sequence;  /* sequence number for synthetic stress-test events; also carries the newly
                         * created Connection's id for CONNECTION_OPENED once
                         * listener_manager_process_events() has applied it (0 before then) */
    char message[128];  /* human-readable detail (e.g. START_FAILED's reason); empty ("") otherwise */

    /* STARTED payload - informational only, nothing currently reads
     * these back out of the event. */
    int socket_fd;
    time_t started_at;

    /* CONNECTION_OPENED payload. socket_fd above doubles as the
     * accepted connection's fd here (STARTED and CONNECTION_OPENED
     * never share a single event, so there's no ambiguity in reusing
     * the field). */
    char remote_host[64];
    uint16_t remote_port;
    void *tls; /* CONNECTION_OPENED only - NULL unless the accepting listener was
                * HTTPS, in which case this is the handshake-completed SSL* crossing
                * from the listener's worker thread to the GUI thread's event
                * processing, opaque here for the same reason connection.h's is. */
} ListenerEvent;

#endif /* WORKBENCH_LISTENER_EVENT_H */
