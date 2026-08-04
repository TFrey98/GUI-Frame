#include "file_watch_queue.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct FileWatchNode {
    FileWatchEvent event;
    struct FileWatchNode *next;
} FileWatchNode;

/* Singly-linked FIFO guarded by one mutex - same shape as
 * event_queue.c's EventQueue. Filesystem event volume here is
 * human-interaction speed, not a hot path, so a single lock is simple
 * and sufficient. */
struct FileWatchQueue {
    pthread_mutex_t lock;
    FileWatchNode *head;
    FileWatchNode *tail;
};

FileWatchQueue *file_watch_queue_create(void) {
    FileWatchQueue *queue = malloc(sizeof(FileWatchQueue));
    pthread_mutex_init(&queue->lock, NULL);
    queue->head = NULL;
    queue->tail = NULL;
    return queue;
}

void file_watch_queue_destroy(FileWatchQueue *queue) {
    if (!queue) {
        return;
    }
    FileWatchNode *node = queue->head;
    while (node) {
        FileWatchNode *next = node->next;
        free(node);
        node = next;
    }
    pthread_mutex_destroy(&queue->lock);
    free(queue);
}

void file_watch_queue_push(FileWatchQueue *queue, FileWatchEvent event) {
    FileWatchNode *node = malloc(sizeof(FileWatchNode));
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

int file_watch_queue_try_pop(FileWatchQueue *queue, FileWatchEvent *out) {
    pthread_mutex_lock(&queue->lock);
    FileWatchNode *node = queue->head;
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
