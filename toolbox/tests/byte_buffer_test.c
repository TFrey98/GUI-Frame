/*
 * Round-trips data through ByteBuffer and checks it byte-for-byte against
 * a plain append-only reference array with its own read cursor - since
 * ByteBuffer's consume() always returns the oldest unread bytes first,
 * "consume the next N bytes" is equivalent to "read the next N bytes of
 * everything ever appended", which is trivial to model that way.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "listeners/byte_buffer.h"

/* Deterministic, easy-to-eyeball content: an incrementing ramp mod 256. */
static void fill_ramp(unsigned char *out, size_t len, unsigned int *counter) {
    for (size_t i = 0; i < len; i++) {
        out[i] = (unsigned char)((*counter)++);
    }
}

static int round_trip_test(void) {
    int status = 0;
    unsigned int counter = 0;

    /* Small initial capacity so the interleaved sizes below force both
     * compaction and growth, not just a single allocation. */
    ByteBuffer *buf = byte_buffer_create(4);

    unsigned char *reference = NULL;
    size_t reference_len = 0;
    size_t reference_pos = 0;

    size_t appends[] = {0, 1, 10, 5, 300};
    size_t consumes[] = {0, 3, 100, 250};
    size_t append_i = 0, consume_i = 0;

    /* Alternate append/consume until both lists are exhausted, so growth
     * (appends beyond current capacity) and partial/over-consumption both
     * get exercised against the same buffer. */
    while (append_i < sizeof(appends) / sizeof(appends[0]) || consume_i < sizeof(consumes) / sizeof(consumes[0])) {
        if (append_i < sizeof(appends) / sizeof(appends[0])) {
            size_t len = appends[append_i++];
            unsigned char *chunk = malloc(len ? len : 1);
            fill_ramp(chunk, len, &counter);
            byte_buffer_append(buf, chunk, len);

            reference = realloc(reference, reference_len + len);
            memcpy(reference + reference_len, chunk, len);
            reference_len += len;
            free(chunk);
        }

        if (consume_i < sizeof(consumes) / sizeof(consumes[0])) {
            size_t max_len = consumes[consume_i++];
            unsigned char *out = malloc(max_len ? max_len : 1);
            size_t n = byte_buffer_consume(buf, out, max_len);

            size_t available = reference_len - reference_pos;
            size_t expected_n = max_len < available ? max_len : available;
            if (n != expected_n) {
                fprintf(stderr, "byte_buffer_test: consume(%zu) returned %zu, expected %zu\n", max_len, n, expected_n);
                status = 1;
            } else if (memcmp(out, reference + reference_pos, n) != 0) {
                fprintf(stderr, "byte_buffer_test: consumed bytes did not match reference at offset %zu\n", reference_pos);
                status = 1;
            }
            reference_pos += n;
            free(out);
        }
    }

    size_t remaining = reference_len - reference_pos;
    if (byte_buffer_len(buf) != remaining) {
        fprintf(stderr, "byte_buffer_test: byte_buffer_len() = %zu, expected %zu\n", byte_buffer_len(buf), remaining);
        status = 1;
    }

    /* Drain whatever's left and check it too. */
    unsigned char *tail = malloc(remaining ? remaining : 1);
    size_t n = byte_buffer_consume(buf, tail, remaining);
    if (n != remaining || (remaining > 0 && memcmp(tail, reference + reference_pos, remaining) != 0)) {
        fprintf(stderr, "byte_buffer_test: final drain did not match remaining reference bytes\n");
        status = 1;
    }
    free(tail);

    byte_buffer_destroy(buf);
    free(reference);
    return status;
}

static int clear_test(void) {
    int status = 0;
    ByteBuffer *buf = byte_buffer_create(0);

    unsigned char data[32];
    unsigned int counter = 0;
    fill_ramp(data, sizeof(data), &counter);
    byte_buffer_append(buf, data, sizeof(data));

    if (byte_buffer_len(buf) != sizeof(data)) {
        fprintf(stderr, "byte_buffer_test: expected len %zu before clear, got %zu\n", sizeof(data), byte_buffer_len(buf));
        status = 1;
    }

    byte_buffer_clear(buf);

    if (byte_buffer_len(buf) != 0) {
        fprintf(stderr, "byte_buffer_test: expected len 0 after clear, got %zu\n", byte_buffer_len(buf));
        status = 1;
    }

    unsigned char scratch[8];
    size_t n = byte_buffer_consume(buf, scratch, sizeof(scratch));
    if (n != 0) {
        fprintf(stderr, "byte_buffer_test: consume after clear returned %zu, expected 0\n", n);
        status = 1;
    }

    /* Buffer should still work normally after being cleared. */
    fill_ramp(data, sizeof(data), &counter);
    byte_buffer_append(buf, data, sizeof(data));
    if (byte_buffer_len(buf) != sizeof(data)) {
        fprintf(stderr, "byte_buffer_test: append after clear did not land, len = %zu\n", byte_buffer_len(buf));
        status = 1;
    }

    byte_buffer_destroy(buf);
    return status;
}

static int peek_and_discard_test(void) {
    int status = 0;
    unsigned int counter = 0;
    unsigned char data[64];
    fill_ramp(data, sizeof(data), &counter);

    ByteBuffer *buf = byte_buffer_create(0);
    byte_buffer_append(buf, data, sizeof(data));

    unsigned char peeked[20];
    size_t n = byte_buffer_peek(buf, peeked, sizeof(peeked));
    if (n != sizeof(peeked) || memcmp(peeked, data, sizeof(peeked)) != 0) {
        fprintf(stderr, "byte_buffer_test: peek did not return the expected leading bytes\n");
        status = 1;
    }
    if (byte_buffer_len(buf) != sizeof(data)) {
        fprintf(stderr, "byte_buffer_test: peek should not remove bytes, but len changed\n");
        status = 1;
    }

    /* Simulate a partial write(): only part of what was peeked actually
     * "went out", so only that much should be discarded. */
    size_t partial = 7;
    size_t discarded = byte_buffer_discard(buf, partial);
    if (discarded != partial || byte_buffer_len(buf) != sizeof(data) - partial) {
        fprintf(stderr, "byte_buffer_test: discard(%zu) did not remove exactly that many bytes\n", partial);
        status = 1;
    }

    unsigned char remaining[64];
    size_t remaining_n = byte_buffer_peek(buf, remaining, sizeof(remaining));
    if (remaining_n != sizeof(data) - partial || memcmp(remaining, data + partial, remaining_n) != 0) {
        fprintf(stderr, "byte_buffer_test: bytes after a partial discard did not match the expected remainder\n");
        status = 1;
    }

    /* Discarding more than what's available should clamp to the
     * available bytes, not underflow. */
    size_t over_discarded = byte_buffer_discard(buf, 10000);
    if (over_discarded != remaining_n || byte_buffer_len(buf) != 0) {
        fprintf(stderr, "byte_buffer_test: over-discard did not clamp to the available bytes\n");
        status = 1;
    }

    byte_buffer_destroy(buf);
    return status;
}

#define STRESS_CHUNK_SIZE 37
#define STRESS_CHUNK_COUNT 500
#define STRESS_TOTAL_BYTES (STRESS_CHUNK_SIZE * STRESS_CHUNK_COUNT)

static void *stress_producer(void *arg) {
    ByteBuffer *buf = arg;
    unsigned int counter = 0;
    for (int i = 0; i < STRESS_CHUNK_COUNT; i++) {
        unsigned char chunk[STRESS_CHUNK_SIZE];
        fill_ramp(chunk, sizeof(chunk), &counter);
        byte_buffer_append(buf, chunk, sizeof(chunk));
    }
    return NULL;
}

/*
 * One producer thread appends while this thread concurrently
 * peek()s+discard()s, verifying the drained bytes exactly match the
 * continuous ramp the producer is writing - proving the internal mutex
 * (added this phase, since "outgoing"/"incoming" both genuinely cross
 * the GUI-thread/worker-thread boundary) serializes correctly with no
 * lost, duplicated, or corrupted bytes. A single producer + single
 * consumer on a FIFO means byte order is fully determined regardless of
 * how the two threads interleave, so any deviation is a real bug.
 */
static int concurrency_stress_test(void) {
    int status = 0;
    ByteBuffer *buf = byte_buffer_create(16); /* small - forces growth/compaction under contention */

    pthread_t producer;
    pthread_create(&producer, NULL, stress_producer, buf);

    unsigned int expected_counter = 0;
    size_t total_consumed = 0;
    int stall_ms = 0;
    const int max_stall_ms = 5000;

    while (total_consumed < STRESS_TOTAL_BYTES) {
        unsigned char chunk[64];
        size_t n = byte_buffer_peek(buf, chunk, sizeof(chunk));
        if (n == 0) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 1 * 1000 * 1000};
            nanosleep(&ts, NULL);
            if (++stall_ms > max_stall_ms) {
                fprintf(stderr, "byte_buffer_test: concurrency stress stalled waiting for data\n");
                status = 1;
                break;
            }
            continue;
        }
        stall_ms = 0;

        for (size_t i = 0; i < n; i++) {
            if (chunk[i] != (unsigned char)(expected_counter++)) {
                fprintf(stderr, "byte_buffer_test: concurrency stress byte mismatch at offset %zu\n",
                        total_consumed + i);
                status = 1;
                break;
            }
        }
        if (status != 0) {
            break;
        }

        size_t discarded = byte_buffer_discard(buf, n);
        if (discarded != n) {
            fprintf(stderr, "byte_buffer_test: discard after peek returned a different count than peeked\n");
            status = 1;
            break;
        }
        total_consumed += n;
    }

    pthread_join(producer, NULL);

    if (status == 0 && total_consumed != STRESS_TOTAL_BYTES) {
        fprintf(stderr, "byte_buffer_test: concurrency stress consumed %zu bytes, expected %d\n", total_consumed,
                STRESS_TOTAL_BYTES);
        status = 1;
    }

    byte_buffer_destroy(buf);
    return status;
}

int main(void) {
    int status = 0;
    status |= round_trip_test();
    status |= clear_test();
    status |= peek_and_discard_test();
    status |= concurrency_stress_test();

    if (status == 0) {
        printf("byte_buffer_test: interleaved append/consume/peek/discard round-tripped byte-for-byte, "
               "including under concurrent access\n");
    }
    return status;
}
