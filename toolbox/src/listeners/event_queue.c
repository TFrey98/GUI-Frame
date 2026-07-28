#include "event_queue.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct EventNode {
    ListenerEvent event;
    struct EventNode *next;
} EventNode;

/* Singly-linked FIFO guarded by one mutex: tail-append on push, head-pop
 * on pop. Event volume here is human/network-interaction speed, not a hot
 * path, so a single lock is simple and sufficient - no need for a
 * lock-free ring buffer. */
struct EventQueue {
    pthread_mutex_t lock;
    EventNode *head;
    EventNode *tail;
};

EventQueue *event_queue_create(void) {
    EventQueue *queue = malloc(sizeof(EventQueue));
    pthread_mutex_init(&queue->lock, NULL);
    queue->head = NULL;
    queue->tail = NULL;
    return queue;
}

void event_queue_destroy(EventQueue *queue) {
    if (!queue) {
        return;
    }
    EventNode *node = queue->head;
    while (node) {
        EventNode *next = node->next;
        free(node);
        node = next;
    }
    pthread_mutex_destroy(&queue->lock);
    free(queue);
}

void event_queue_push(EventQueue *queue, ListenerEvent event) {
    EventNode *node = malloc(sizeof(EventNode));
    node->event = event;
    node->next = NULL;

    pthread_mutex_lock(&queue->lock);
    if (queue->tail) {
        queue->tail->next = node;
    } else {
        queue->head = node;
    }
    queue->tail = node;
    pthread_mutex_unlock(&queue->lock);
}

int event_queue_try_pop(EventQueue *queue, ListenerEvent *out) {
    pthread_mutex_lock(&queue->lock);
    EventNode *node = queue->head;
    if (!node) {
        pthread_mutex_unlock(&queue->lock);
        return 0;
    }
    queue->head = node->next;
    if (!queue->head) {
        queue->tail = NULL;
    }
    pthread_mutex_unlock(&queue->lock);

    *out = node->event;
    free(node);
    return 1;
}
