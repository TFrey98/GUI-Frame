#include "line_accumulator.h"

#include <stdlib.h>
#include <string.h>

#define LINE_ACCUMULATOR_DEFAULT_CAPACITY 128

struct LineAccumulator {
    char *buf;
    size_t len;
    size_t capacity;
    /* True right after a line-terminating '\r' was consumed, so a '\n'
     * arriving as the very first byte of a later feed() call is swallowed
     * as part of the same CRLF pair instead of producing an empty line. */
    int pending_crlf;
};

LineAccumulator *line_accumulator_create(void) {
    LineAccumulator *acc = malloc(sizeof(LineAccumulator));
    acc->buf = malloc(LINE_ACCUMULATOR_DEFAULT_CAPACITY);
    acc->len = 0;
    acc->capacity = LINE_ACCUMULATOR_DEFAULT_CAPACITY;
    acc->pending_crlf = 0;
    return acc;
}

void line_accumulator_destroy(LineAccumulator *acc) {
    if (!acc) {
        return;
    }
    free(acc->buf);
    free(acc);
}

static void append_byte(LineAccumulator *acc, char c) {
    if (acc->len == acc->capacity) {
        acc->capacity *= 2;
        acc->buf = realloc(acc->buf, acc->capacity);
    }
    acc->buf[acc->len++] = c;
}

void line_accumulator_feed(LineAccumulator *acc, const char *data, size_t len,
                            void (*on_line)(const char *line, size_t len, void *user_data), void *user_data) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];

        if (acc->pending_crlf) {
            acc->pending_crlf = 0;
            if (c == '\n') {
                continue; /* second half of a CRLF pair split across feed() calls */
            }
        }

        if (c == '\n' || c == '\r') {
            /* An empty line (e.g. every buffered character was erased by
             * backspace, or the user just pressed Enter with nothing
             * typed) isn't a command - nothing meaningful to capture. */
            if (acc->len > 0) {
                on_line(acc->buf, acc->len, user_data);
            }
            acc->len = 0;
            if (c == '\r') {
                acc->pending_crlf = 1;
            }
            continue;
        }

        if (c == 0x08 || c == 0x7f) {
            /* Backspace/DEL is a line-editing operation, not text: erase
             * the previously buffered byte (if any) rather than embedding
             * the raw control byte. Without this, the captured line would
             * literally contain backspace characters instead of reflecting
             * what the user actually ended up submitting after corrections. */
            if (acc->len > 0) {
                acc->len--;
            }
            continue;
        }

        if (c < 0x20) {
            /* Other C0 control bytes (Escape, Tab, Ctrl+<letter>, ...) -
             * typically arrow-key/completion/history-recall sequences or
             * signal keys, not literal command text. Dropped rather than
             * embedded raw; a captured line stays readable even though
             * this doesn't attempt to fully replay what a receiving
             * readline would have done with them. */
            continue;
        }

        append_byte(acc, (char)c);
    }
}
