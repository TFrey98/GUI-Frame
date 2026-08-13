/*
 * FileWatchQueue's basic FIFO contract (push/pop order, empty-queue
 * behavior) plus a stress variant mirroring event_queue_stress_test.c's
 * own shape - many producer threads push concurrently while this thread
 * drains via try_pop, verifying nothing is lost, duplicated, or
 * corrupted and each producer's own events arrive complete and in the
 * order it pushed them.
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "files/file_watch_queue.h"

static int test_basic_fifo_order(void) {
    FileWatchQueue *queue = file_watch_queue_create();

    FileWatchEvent out;
    if (file_watch_queue_try_pop(queue, &out)) {
        fprintf(stderr, "file_watch_queue_test: try_pop on an empty queue should return 0\n");
        file_watch_queue_destroy(queue);
        return 1;
    }

    FileWatchEvent a = {0};
    a.type = FILE_WATCH_CREATED;
    snprintf(a.new_relative_path, sizeof(a.new_relative_path), "a.txt");
    FileWatchEvent b = {0};
    b.type = FILE_WATCH_DELETED;
    snprintf(b.new_relative_path, sizeof(b.new_relative_path), "b.txt");
    FileWatchEvent c = {0};
    c.type = FILE_WATCH_RENAMED;
    snprintf(c.old_relative_path, sizeof(c.old_relative_path), "old.txt");
    snprintf(c.new_relative_path, sizeof(c.new_relative_path), "new.txt");

    file_watch_queue_push(queue, a);
    file_watch_queue_push(queue, b);
    file_watch_queue_push(queue, c);

    int status = 0;
    if (!file_watch_queue_try_pop(queue, &out) || out.type != FILE_WATCH_CREATED ||
        strcmp(out.new_relative_path, "a.txt") != 0) {
        fprintf(stderr, "file_watch_queue_test: expected 'a.txt' created first\n");
        status = 1;
    }
    if (!file_watch_queue_try_pop(queue, &out) || out.type != FILE_WATCH_DELETED ||
        strcmp(out.new_relative_path, "b.txt") != 0) {
        fprintf(stderr, "file_watch_queue_test: expected 'b.txt' deleted second\n");
        status = 1;
    }
    if (!file_watch_queue_try_pop(queue, &out) || out.type != FILE_WATCH_RENAMED ||
        strcmp(out.old_relative_path, "old.txt") != 0 || strcmp(out.new_relative_path, "new.txt") != 0) {
        fprintf(stderr, "file_watch_queue_test: expected 'old.txt' -> 'new.txt' renamed third\n");
        status = 1;
    }
    if (file_watch_queue_try_pop(queue, &out)) {
        fprintf(stderr, "file_watch_queue_test: queue should be empty after draining exactly 3 pushes\n");
        status = 1;
    }

    file_watch_queue_destroy(queue);
    return status;
}

#define PRODUCER_COUNT 8
#define EVENTS_PER_PRODUCER 1000

typedef struct ProducerArgs {
    FileWatchQueue *queue;
    int producer_id;
} ProducerArgs;

static void *producer_thread(void *arg) {
    ProducerArgs *args = arg;
    for (int i = 0; i < EVENTS_PER_PRODUCER; i++) {
        FileWatchEvent event = {0};
        event.type = FILE_WATCH_MODIFIED;
        /* Encodes producer id + sequence into the path so the drain loop
         * can verify per-producer FIFO order without a separate field. */
        snprintf(event.new_relative_path, sizeof(event.new_relative_path), "producer-%d/%d", args->producer_id, i);
        file_watch_queue_push(args->queue, event);
    }
    return NULL;
}

typedef struct JoinerArgs {
    pthread_t *producers;
    atomic_int *producers_done;
} JoinerArgs;

static void *joiner_thread(void *arg) {
    JoinerArgs *args = arg;
    for (int i = 0; i < PRODUCER_COUNT; i++) {
        pthread_join(args->producers[i], NULL);
    }
    atomic_store(args->producers_done, 1);
    return NULL;
}

static int test_concurrent_stress(void) {
    FileWatchQueue *queue = file_watch_queue_create();

    pthread_t producers[PRODUCER_COUNT];
    ProducerArgs producer_args[PRODUCER_COUNT];
    for (int i = 0; i < PRODUCER_COUNT; i++) {
        producer_args[i].queue = queue;
        producer_args[i].producer_id = i;
        pthread_create(&producers[i], NULL, producer_thread, &producer_args[i]);
    }

    atomic_int producers_done = 0;
    JoinerArgs joiner_args = {producers, &producers_done};
    pthread_t joiner;
    pthread_create(&joiner, NULL, joiner_thread, &joiner_args);

    int expected_seq[PRODUCER_COUNT] = {0};
    int counts[PRODUCER_COUNT] = {0};
    long total = 0;
    int status = 0;

    for (;;) {
        FileWatchEvent event;
        if (file_watch_queue_try_pop(queue, &event)) {
            int pid = -1, seq = -1;
            sscanf(event.new_relative_path, "producer-%d/%d", &pid, &seq);
            if (pid < 0 || pid >= PRODUCER_COUNT) {
                fprintf(stderr, "file_watch_queue_test: unexpected producer id in '%s'\n", event.new_relative_path);
                status = 1;
                continue;
            }
            if (seq != expected_seq[pid]) {
                fprintf(stderr, "file_watch_queue_test: producer %d out of order (expected %d, got %d)\n", pid,
                        expected_seq[pid], seq);
                status = 1;
            }
            expected_seq[pid] = seq + 1;
            counts[pid]++;
            total++;
        } else if (atomic_load(&producers_done)) {
            break;
        } else {
            sched_yield();
        }
    }

    /* Same reasoning as event_queue_stress_test.c: a push can land in the
     * gap between this thread's last failed try_pop and its done check,
     * so drain unconditionally once every producer has provably
     * finished. */
    FileWatchEvent event;
    while (file_watch_queue_try_pop(queue, &event)) {
        int pid = -1, seq = -1;
        sscanf(event.new_relative_path, "producer-%d/%d", &pid, &seq);
        if (seq != expected_seq[pid]) {
            status = 1;
        }
        expected_seq[pid] = seq + 1;
        counts[pid]++;
        total++;
    }

    pthread_join(joiner, NULL);

    if (total != (long)PRODUCER_COUNT * EVENTS_PER_PRODUCER) {
        fprintf(stderr, "file_watch_queue_test: expected %d events total, got %ld\n",
                PRODUCER_COUNT * EVENTS_PER_PRODUCER, total);
        status = 1;
    }
    for (int i = 0; i < PRODUCER_COUNT; i++) {
        if (counts[i] != EVENTS_PER_PRODUCER) {
            fprintf(stderr, "file_watch_queue_test: producer %d delivered %d/%d events\n", i, counts[i],
                    EVENTS_PER_PRODUCER);
            status = 1;
        }
    }

    file_watch_queue_destroy(queue);
    return status;
}

int main(void) {
    int status = 0;
    status |= test_basic_fifo_order();
    status |= test_concurrent_stress();

    if (status == 0) {
        printf("file_watch_queue_test: FIFO order, empty-queue behavior, and concurrent stress all verified\n");
    }
    return status;
}
