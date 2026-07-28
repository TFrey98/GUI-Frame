#include "terminal_history.h"

#include <stdlib.h>
#include <string.h>

#define TERMINAL_HISTORY_DEFAULT_CAPACITY 256

struct TerminalHistory {
    unsigned char *data;
    size_t capacity;
    size_t len;
};

TerminalHistory *terminal_history_create(void) {
    TerminalHistory *history = malloc(sizeof(TerminalHistory));
    history->data = malloc(TERMINAL_HISTORY_DEFAULT_CAPACITY);
    history->capacity = TERMINAL_HISTORY_DEFAULT_CAPACITY;
    history->len = 0;
    return history;
}

void terminal_history_destroy(TerminalHistory *history) {
    if (!history) {
        return;
    }
    free(history->data);
    free(history);
}

void terminal_history_append(TerminalHistory *history, const void *data, size_t len) {
    if (len == 0) {
        return;
    }
    if (history->len + len > history->capacity) {
        size_t new_capacity = history->capacity == 0 ? TERMINAL_HISTORY_DEFAULT_CAPACITY : history->capacity;
        while (new_capacity < history->len + len) {
            new_capacity *= 2;
        }
        history->data = realloc(history->data, new_capacity);
        history->capacity = new_capacity;
    }
    memcpy(history->data + history->len, data, len);
    history->len += len;
}

size_t terminal_history_len(const TerminalHistory *history) {
    return history->len;
}

size_t terminal_history_read(const TerminalHistory *history, size_t offset, void *dst, size_t max_len) {
    if (offset >= history->len) {
        return 0;
    }
    size_t available = history->len - offset;
    size_t n = max_len < available ? max_len : available;
    memcpy(dst, history->data + offset, n);
    return n;
}
