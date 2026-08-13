#include "terminal_history.h"

#include <stdlib.h>
#include <string.h>

#include "ansi_strip.h"
#include "database.h"

#define TERMINAL_HISTORY_DEFAULT_CAPACITY 256

struct TerminalHistory {
    unsigned char *data;
    size_t capacity;
    size_t len;
    char terminal_kind[16];
    uint64_t terminal_id;
    /* Only ever fed while persist_to_db is true (see terminal_history_append)
     * - strips color/cursor escape sequences from the copy that goes to the
     * database, so a captured/exported row reads as plain text, without
     * touching `data` above (display/replay still needs the real bytes). */
    AnsiStripper *ansi_stripper;
};

TerminalHistory *terminal_history_create(const char *terminal_kind, uint64_t terminal_id) {
    TerminalHistory *history = malloc(sizeof(TerminalHistory));
    history->data = malloc(TERMINAL_HISTORY_DEFAULT_CAPACITY);
    history->capacity = TERMINAL_HISTORY_DEFAULT_CAPACITY;
    history->len = 0;
    strncpy(history->terminal_kind, terminal_kind, sizeof(history->terminal_kind) - 1);
    history->terminal_kind[sizeof(history->terminal_kind) - 1] = '\0';
    history->terminal_id = terminal_id;
    history->ansi_stripper = ansi_stripper_create();
    return history;
}

void terminal_history_destroy(TerminalHistory *history) {
    if (!history) {
        return;
    }
    ansi_stripper_destroy(history->ansi_stripper);
    free(history->data);
    free(history);
}

void terminal_history_append(TerminalHistory *history, const void *data, size_t len, bool persist_to_db) {
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

    if (!persist_to_db) {
        /* Not persisting this chunk - any escape sequence still pending
         * from a previous persisted chunk belongs to a now-abandoned
         * capture window and must not bleed into a future, unrelated one. */
        ansi_stripper_reset(history->ansi_stripper);
        return;
    }

    unsigned char *stripped = NULL;
    size_t stripped_len = 0;
    ansi_stripper_feed(history->ansi_stripper, data, len, &stripped, &stripped_len);
    if (stripped_len > 0) {
        database_record_terminal_event(history->terminal_kind, history->terminal_id, "output", stripped,
                                        stripped_len);
    }
    free(stripped);
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
