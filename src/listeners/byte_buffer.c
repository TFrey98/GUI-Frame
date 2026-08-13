#include "byte_buffer.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define BYTE_BUFFER_DEFAULT_CAPACITY 256

/* [data + head, data + tail) is the unread region; bytes before head have
 * already been consumed. head/tail both reset to 0 once fully drained so
 * a buffer that's regularly emptied doesn't creep toward its capacity.
 * lock guards every field above - see the header for why this needs to
 * be thread-safe. */
struct ByteBuffer {
    unsigned char *data;
    size_t capacity;
    size_t head;
    size_t tail;
    pthread_mutex_t lock;
};

ByteBuffer *byte_buffer_create(size_t initial_capacity) {
    if (initial_capacity == 0) {
        initial_capacity = BYTE_BUFFER_DEFAULT_CAPACITY;
    }
    ByteBuffer *buf = malloc(sizeof(ByteBuffer));
    buf->data = malloc(initial_capacity);
    buf->capacity = initial_capacity;
    buf->head = 0;
    buf->tail = 0;
    pthread_mutex_init(&buf->lock, NULL);
    return buf;
}

void byte_buffer_destroy(ByteBuffer *buf) {
    if (!buf) {
        return;
    }
    pthread_mutex_destroy(&buf->lock);
    free(buf->data);
    free(buf);
}

/* Internal, lock-already-held variants - the public functions below
 * take buf->lock once and call these, and byte_buffer_consume composes
 * peek+discard without locking twice. */

static void append_locked(ByteBuffer *buf, const void *data, size_t len) {
    if (len == 0) {
        return;
    }

    size_t used = buf->tail - buf->head;

    /* Prefer compacting the existing unread bytes back to the start of
     * the buffer over growing it, if that alone makes room. */
    if (buf->head > 0 && used + len <= buf->capacity) {
        memmove(buf->data, buf->data + buf->head, used);
        buf->head = 0;
        buf->tail = used;
    }

    if (buf->tail + len > buf->capacity) {
        size_t new_capacity = buf->capacity == 0 ? BYTE_BUFFER_DEFAULT_CAPACITY : buf->capacity;
        while (new_capacity < used + len) {
            new_capacity *= 2;
        }
        buf->data = realloc(buf->data, new_capacity);
        buf->capacity = new_capacity;
    }

    memcpy(buf->data + buf->tail, data, len);
    buf->tail += len;
}

static size_t peek_locked(const ByteBuffer *buf, void *dst, size_t max_len) {
    size_t available = buf->tail - buf->head;
    size_t n = max_len < available ? max_len : available;
    memcpy(dst, buf->data + buf->head, n);
    return n;
}

static size_t discard_locked(ByteBuffer *buf, size_t len) {
    size_t available = buf->tail - buf->head;
    size_t n = len < available ? len : available;
    buf->head += n;
    if (buf->head == buf->tail) {
        buf->head = 0;
        buf->tail = 0;
    }
    return n;
}

void byte_buffer_append(ByteBuffer *buf, const void *data, size_t len) {
    pthread_mutex_lock(&buf->lock);
    append_locked(buf, data, len);
    pthread_mutex_unlock(&buf->lock);
}

size_t byte_buffer_consume(ByteBuffer *buf, void *dst, size_t max_len) {
    pthread_mutex_lock(&buf->lock);
    size_t n = peek_locked(buf, dst, max_len);
    discard_locked(buf, n);
    pthread_mutex_unlock(&buf->lock);
    return n;
}

size_t byte_buffer_peek(ByteBuffer *buf, void *dst, size_t max_len) {
    pthread_mutex_lock(&buf->lock);
    size_t n = peek_locked(buf, dst, max_len);
    pthread_mutex_unlock(&buf->lock);
    return n;
}

size_t byte_buffer_discard(ByteBuffer *buf, size_t len) {
    pthread_mutex_lock(&buf->lock);
    size_t n = discard_locked(buf, len);
    pthread_mutex_unlock(&buf->lock);
    return n;
}

size_t byte_buffer_len(const ByteBuffer *buf) {
    /* Logically const (doesn't change the buffer's visible content),
     * but still needs the lock for a consistent read under concurrent
     * access - the mutex itself is the only thing being mutated here. */
    pthread_mutex_lock((pthread_mutex_t *)&buf->lock);
    size_t len = buf->tail - buf->head;
    pthread_mutex_unlock((pthread_mutex_t *)&buf->lock);
    return len;
}

void byte_buffer_clear(ByteBuffer *buf) {
    pthread_mutex_lock(&buf->lock);
    buf->head = 0;
    buf->tail = 0;
    pthread_mutex_unlock(&buf->lock);
}
