/*
 * Stresses EventQueue's thread-safety: many producer threads push
 * concurrently while this thread drains via try_pop, matching the real
 * usage shape (worker threads push, GUI thread drains). Verifies nothing
 * is lost, duplicated, or corrupted by checking that every event arrives
 * and each producer's own sequence numbers arrive complete and in order
 * (cross-producer interleaving order is not asserted, only per-producer
 * FIFO order, which is all EventQueue promises).
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>

#include "listeners/event_queue.h"

#define PRODUCER_COUNT 8
#define EVENTS_PER_PRODUCER 1000

typedef struct ProducerArgs {
    EventQueue *queue;
    uint64_t producer_id;
} ProducerArgs;

static void *producer_thread(void *arg) {
    ProducerArgs *args = arg;
    for (uint64_t i = 0; i < EVENTS_PER_PRODUCER; i++) {
        ListenerEvent event = {
            .type = LISTENER_EVENT_TEST,
            .object_id = args->producer_id,
            .sequence = i,
        };
        event_queue_push(args->queue, event);
    }
    return NULL;
}

typedef struct JoinerArgs {
    pthread_t *producers;
    atomic_int *producers_done;
} JoinerArgs;

/* Joins every producer thread (blocking until all pushes are complete),
 * then flips producers_done - the drain loop below uses this instead of
 * joining producers itself so it can keep draining the queue instead of
 * blocking on pthread_join. */
static void *joiner_thread(void *arg) {
    JoinerArgs *args = arg;
    for (int i = 0; i < PRODUCER_COUNT; i++) {
        pthread_join(args->producers[i], NULL);
    }
    atomic_store(args->producers_done, 1);
    return NULL;
}

int main(void) {
    EventQueue *queue = event_queue_create();

    pthread_t producers[PRODUCER_COUNT];
    ProducerArgs producer_args[PRODUCER_COUNT];
    for (int i = 0; i < PRODUCER_COUNT; i++) {
        producer_args[i].queue = queue;
        producer_args[i].producer_id = (uint64_t)i;
        pthread_create(&producers[i], NULL, producer_thread, &producer_args[i]);
    }

    atomic_int producers_done = 0;
    JoinerArgs joiner_args = { producers, &producers_done };
    pthread_t joiner;
    pthread_create(&joiner, NULL, joiner_thread, &joiner_args);

    uint64_t expected_seq[PRODUCER_COUNT] = {0};
    uint64_t counts[PRODUCER_COUNT] = {0};
    long total = 0;
    int status = 0;

    for (int i = 0; i < PRODUCER_COUNT; i++) {
        expected_seq[i] = 0;
        counts[i] = 0;
    }

    for (;;) {
        ListenerEvent event;
        if (event_queue_try_pop(queue, &event)) {
            total++;
            uint64_t pid = event.object_id;
            if (pid >= PRODUCER_COUNT) {
                fprintf(stderr, "event_queue_stress_test: unexpected object_id %llu\n",
                        (unsigned long long)pid);
                status = 1;
                continue;
            }
            if (event.sequence != expected_seq[pid]) {
                fprintf(stderr,
                        "event_queue_stress_test: producer %llu out of order (expected %llu, got %llu)\n",
                        (unsigned long long)pid, (unsigned long long)expected_seq[pid],
                        (unsigned long long)event.sequence);
                status = 1;
            }
            expected_seq[pid] = event.sequence + 1;
            counts[pid]++;
        } else if (atomic_load(&producers_done)) {
            break;
        } else {
            sched_yield();
        }
    }

    /* A push can land in the gap between this thread's last failed
     * try_pop and its done check above (this thread can be preempted
     * between the two) - drain unconditionally now that every producer
     * has provably finished, to be sure nothing pushed during that gap
     * was missed. */
    ListenerEvent event;
    while (event_queue_try_pop(queue, &event)) {
        total++;
        uint64_t pid = event.object_id;
        if (event.sequence != expected_seq[pid]) {
            fprintf(stderr,
                    "event_queue_stress_test: producer %llu out of order in final drain (expected %llu, got %llu)\n",
                    (unsigned long long)pid, (unsigned long long)expected_seq[pid],
                    (unsigned long long)event.sequence);
            status = 1;
        }
        expected_seq[pid] = event.sequence + 1;
        counts[pid]++;
    }

    pthread_join(joiner, NULL);

    if (total != (long)PRODUCER_COUNT * EVENTS_PER_PRODUCER) {
        fprintf(stderr, "event_queue_stress_test: expected %d events total, got %ld\n",
                PRODUCER_COUNT * EVENTS_PER_PRODUCER, total);
        status = 1;
    }
    for (int i = 0; i < PRODUCER_COUNT; i++) {
        if (counts[i] != EVENTS_PER_PRODUCER) {
            fprintf(stderr, "event_queue_stress_test: producer %d delivered %llu/%d events\n",
                    i, (unsigned long long)counts[i], EVENTS_PER_PRODUCER);
            status = 1;
        }
    }

    event_queue_destroy(queue);

    if (status == 0) {
        printf("event_queue_stress_test: %ld events from %d producers verified complete and in order\n",
               total, PRODUCER_COUNT);
    }
    return status;
}
