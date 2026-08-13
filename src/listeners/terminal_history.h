#ifndef WORKBENCH_TERMINAL_HISTORY_H
#define WORKBENCH_TERMINAL_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Append-only, offset-readable log of everything a connection or local
 * terminal has received - deliberately a different shape from ByteBuffer
 * (a FIFO that removes on read): history must never lose data once
 * appended, since a reopened terminal view needs to read the same bytes
 * more than once.
 *
 * Lives on the owning Connection (registry) or Terminal (local shell), so
 * - like everything else reachable through those - only the GUI thread
 * ever touches it. Not thread-safe, and doesn't need to be: a worker
 * thread never appends here directly, it only hands off through its own
 * thread-safe "incoming" ByteBuffer, which the GUI thread drains into
 * history each tick.
 *
 * Every append always grows the in-memory buffer (needed unconditionally
 * for display/replay), and optionally also persists to the database as
 * one "output" event tagged with terminal_kind/terminal_id (set at
 * creation) - this is the single point where both connection and
 * local-terminal output get captured, so no caller needs its own database
 * call. The caller decides per-append whether persistence applies (see
 * persist_to_db below) - the intended use is gating it to the window
 * between a submitted command line and the next keystroke, so a typed
 * command's own echo (indistinguishable from real output at the byte
 * level, and not bounded to any particular chunk size for a fast typist)
 * never reaches the database, only the response that follows it.
 */
typedef struct TerminalHistory TerminalHistory;

/* terminal_kind is "local" or "connection"; terminal_id is the owning
 * TerminalSession's or Connection's id. Both are copied (not retained) and
 * attached to every persisted event. */
TerminalHistory *terminal_history_create(const char *terminal_kind, uint64_t terminal_id);
void terminal_history_destroy(TerminalHistory *history);

/* persist_to_db controls only the optional database write - the in-memory
 * append (and thus terminal_history_len()/_read()) always happens. */
void terminal_history_append(TerminalHistory *history, const void *data, size_t len, bool persist_to_db);

/* Total bytes ever appended. */
size_t terminal_history_len(const TerminalHistory *history);

/* Copies up to max_len bytes starting at offset into dst, without
 * removing anything. Returns the number of bytes actually copied (0 if
 * offset is at or past the end). */
size_t terminal_history_read(const TerminalHistory *history, size_t offset, void *dst, size_t max_len);

#endif /* WORKBENCH_TERMINAL_HISTORY_H */
