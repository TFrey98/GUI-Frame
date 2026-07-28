#ifndef TOOLBOX_BYTE_BUFFER_H
#define TOOLBOX_BYTE_BUFFER_H

#include <stddef.h>

/*
 * Growable FIFO byte buffer for connection I/O (incoming/outgoing data,
 * etc.). Internally thread-safe (a single mutex guards every
 * operation) - a connection's "outgoing" buffer is written by the GUI
 * thread and drained by that connection's own worker thread, and
 * "incoming" is the same in reverse, so both genuinely cross the thread
 * boundary.
 */
typedef struct ByteBuffer ByteBuffer;

ByteBuffer *byte_buffer_create(size_t initial_capacity);
void byte_buffer_destroy(ByteBuffer *buf);

/* Appends len bytes, growing capacity as needed. */
void byte_buffer_append(ByteBuffer *buf, const void *data, size_t len);

/* Copies up to max_len unread bytes into dst and removes them from the
 * buffer. Returns the number of bytes actually copied (may be less than
 * max_len if fewer bytes are available). Equivalent to peek() followed
 * by discard() of exactly what was copied. */
size_t byte_buffer_consume(ByteBuffer *buf, void *dst, size_t max_len);

/* Copies up to max_len unread bytes into dst WITHOUT removing them.
 * Pair with byte_buffer_discard() once it's known how much was actually
 * consumed downstream (e.g. a partial write()) - using consume() before
 * that's known would silently drop the un-sent remainder. Returns the
 * number of bytes actually copied. */
size_t byte_buffer_peek(ByteBuffer *buf, void *dst, size_t max_len);

/* Removes up to len already-peeked bytes from the front. Returns the
 * number of bytes actually discarded (may be less than len if fewer
 * bytes are available). */
size_t byte_buffer_discard(ByteBuffer *buf, size_t len);

/* Number of unread bytes currently held. */
size_t byte_buffer_len(const ByteBuffer *buf);

/* Discards all unread bytes without freeing the buffer. */
void byte_buffer_clear(ByteBuffer *buf);

#endif /* TOOLBOX_BYTE_BUFFER_H */
