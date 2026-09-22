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

    /* Open capture window, if any: the escape-stripped bytes seen since
     * it opened, plus the command whose echo is expected at the front. */
    bool capturing;
    unsigned char *pending;
    size_t pending_capacity;
    size_t pending_len;
    char *submitted_line;
    size_t submitted_len;
};

/* Splits off the first line: returns its text length (excluding any
 * terminator) and, via *advance, how many bytes to step over to reach the
 * next line. *advance equals the text length when the line runs to the end
 * of the buffer with no newline at all. */
static size_t first_line(const unsigned char *data, size_t len, size_t *advance) {
    size_t newline = 0;
    while (newline < len && data[newline] != '\n') {
        newline++;
    }
    if (newline == len) {
        *advance = len; /* unterminated */
        return len;
    }
    *advance = newline + 1;
    size_t text_len = newline;
    if (text_len > 0 && data[text_len - 1] == '\r') {
        text_len--; /* the \r belongs to the terminator, not the text */
    }
    return text_len;
}

/* True when the first line is the shell's echo of what was submitted.
 * The echo is whatever the line editor redrew - typically the prompt
 * followed by the command - so the test is that the line *ends* with the
 * submitted text, not that it equals it.
 *
 * What sits between the command and the newline is the line editor
 * tidying the row rather than anything the user typed: spaces padding out
 * to the end of the row, and backspaces walking the cursor back over that
 * padding. Narrowing the terminal makes the editor do more of this, which
 * is why an earlier version of this matched at full width and not when
 * the prompt was truncated. */
static bool first_line_is_echo(const unsigned char *line, size_t line_len, const char *submitted,
                                size_t submitted_len) {
    if (submitted_len == 0 || line_len < submitted_len) {
        return false;
    }
    while (line_len > submitted_len) {
        unsigned char c = line[line_len - 1];
        if (c != ' ' && c != '\t' && c != '\b' && c != '\r') {
            break;
        }
        line_len--;
    }
    return line_len >= submitted_len &&
            memcmp(line + line_len - submitted_len, submitted, submitted_len) == 0;
}

TerminalHistory *terminal_history_create(const char *terminal_kind, uint64_t terminal_id) {
    TerminalHistory *history = malloc(sizeof(TerminalHistory));
    history->data = malloc(TERMINAL_HISTORY_DEFAULT_CAPACITY);
    history->capacity = TERMINAL_HISTORY_DEFAULT_CAPACITY;
    history->len = 0;
    strncpy(history->terminal_kind, terminal_kind, sizeof(history->terminal_kind) - 1);
    history->terminal_kind[sizeof(history->terminal_kind) - 1] = '\0';
    history->terminal_id = terminal_id;
    history->ansi_stripper = ansi_stripper_create();
    history->capturing = false;
    history->pending = NULL;
    history->pending_capacity = 0;
    history->pending_len = 0;
    history->submitted_line = NULL;
    history->submitted_len = 0;
    return history;
}

void terminal_history_destroy(TerminalHistory *history) {
    if (!history) {
        return;
    }
    ansi_stripper_destroy(history->ansi_stripper);
    free(history->pending);
    free(history->submitted_line);
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
        if (history->capturing) {
            /* Held until the window closes, so the echo at the front and
             * the prompt at the back can be trimmed off as a whole. */
            if (history->pending_len + stripped_len > history->pending_capacity) {
                size_t new_capacity = history->pending_capacity == 0 ? TERMINAL_HISTORY_DEFAULT_CAPACITY
                                                                     : history->pending_capacity;
                while (new_capacity < history->pending_len + stripped_len) {
                    new_capacity *= 2;
                }
                history->pending = realloc(history->pending, new_capacity);
                history->pending_capacity = new_capacity;
            }
            memcpy(history->pending + history->pending_len, stripped, stripped_len);
            history->pending_len += stripped_len;
        } else {
            database_record_terminal_event(history->terminal_kind, history->terminal_id, "output", stripped,
                                            stripped_len);
        }
    }
    free(stripped);
}

void terminal_history_begin_capture(TerminalHistory *history, const char *submitted_line, size_t len) {
    if (!history) {
        return;
    }
    terminal_history_end_capture(history);

    history->capturing = true;
    history->pending_len = 0;
    free(history->submitted_line);
    history->submitted_line = NULL;
    history->submitted_len = 0;
    if (submitted_line && len > 0) {
        history->submitted_line = malloc(len);
        memcpy(history->submitted_line, submitted_line, len);
        history->submitted_len = len;
    }
}

void terminal_history_flush_capture(TerminalHistory *history) {
    if (!history || !history->capturing) {
        return;
    }

    const unsigned char *body = history->pending;
    size_t body_len = history->pending_len;

    /* Front: the line editor's redraw of the submitted line, prompt and
     * all. Only dropped when it really is that line, so a command whose
     * output happens to start on the same row is never eaten. Once the
     * echo has been dealt with the expectation is cleared, so a later
     * flush in the same window cannot mistake output for it. */
    if (history->submitted_len > 0) {
        size_t advance = 0;
        size_t first_len = first_line(body, body_len, &advance);
        if (advance < body_len) {
            if (first_line_is_echo(body, first_len, history->submitted_line, history->submitted_len)) {
                body += advance;
                body_len -= advance;
            }
            free(history->submitted_line);
            history->submitted_line = NULL;
            history->submitted_len = 0;
        }
    }

    /* Write only as far as the last newline. Whatever follows it is
     * unterminated - either a prompt, or a line still being written - and
     * is kept for the next flush rather than guessed at now. */
    size_t complete = body_len;
    while (complete > 0 && body[complete - 1] != '\n') {
        complete--;
    }
    if (complete > 0) {
        database_record_terminal_event(history->terminal_kind, history->terminal_id, "output", body, complete);
    }

    size_t tail_len = body_len - complete;
    memmove(history->pending, body + complete, tail_len);
    history->pending_len = tail_len;
}

void terminal_history_end_capture(TerminalHistory *history) {
    if (!history || !history->capturing) {
        return;
    }
    terminal_history_flush_capture(history);
    /* Whatever is left is the prompt the shell is sat at. */
    history->capturing = false;
    history->pending_len = 0;
    free(history->submitted_line);
    history->submitted_line = NULL;
    history->submitted_len = 0;
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
