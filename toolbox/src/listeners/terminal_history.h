#ifndef TOOLBOX_TERMINAL_HISTORY_H
#define TOOLBOX_TERMINAL_HISTORY_H

#include <stddef.h>

/*
 * Append-only, offset-readable log of everything a connection has
 * received - deliberately a different shape from ByteBuffer (a FIFO
 * that removes on read): history must never lose data once appended,
 * since a reopened terminal view needs to read the same bytes more
 * than once.
 *
 * Lives on the registry's Connection, so - like everything else
 * reachable through ObjectRegistry - only the GUI thread ever touches
 * it. Not thread-safe, and doesn't need to be: a connection's worker
 * thread never appends here directly, it only hands off through its
 * own thread-safe "incoming" ByteBuffer, which the GUI thread drains
 * into history each tick (see connection_manager_drain_data()).
 */
typedef struct TerminalHistory TerminalHistory;

TerminalHistory *terminal_history_create(void);
void terminal_history_destroy(TerminalHistory *history);

void terminal_history_append(TerminalHistory *history, const void *data, size_t len);

/* Total bytes ever appended. */
size_t terminal_history_len(const TerminalHistory *history);

/* Copies up to max_len bytes starting at offset into dst, without
 * removing anything. Returns the number of bytes actually copied (0 if
 * offset is at or past the end). */
size_t terminal_history_read(const TerminalHistory *history, size_t offset, void *dst, size_t max_len);

#endif /* TOOLBOX_TERMINAL_HISTORY_H */
