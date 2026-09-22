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
 * between a submitted command line and the next keystroke.
 *
 * Gating alone does not keep a command's own echo out of the capture,
 * which is what it was originally expected to do. A line editor redraws
 * the whole line when Enter is pressed - prompt included - so that redraw
 * arrives after the window has opened, and the recorded "response" began
 * with the prompt and a copy of the command. Narrowing the terminal made
 * it obvious, because the line editor then truncates the prompt and the
 * truncation was recorded too, but it was never about width.
 *
 * So a window holds its bytes back and trims its edges. Complete lines
 * are still written as they arrive, so a command's output reaches the
 * database while it is still running and an export never misses the last
 * command; only the unterminated tail is held, because that is where a
 * prompt lives. The trimming is:
 *
 *   - The echo is removed by matching the submitted line passed to
 *     terminal_history_begin_capture(). That is exact - the caller has
 *     just recorded that line as the "input" event - so it holds for any
 *     prompt, any working directory depth, and whether or not the prompt
 *     was truncated.
 *
 *   - The prompt that follows the response is removed as the trailing
 *     line with no newline after it, since a prompt waits for input on
 *     the line it is drawn on. This one is a judgement rather than a
 *     certainty: output that ends without a trailing newline loses its
 *     last partial line.
 */
typedef struct TerminalHistory TerminalHistory;

/* terminal_kind is "local" or "connection"; terminal_id is the owning
 * TerminalSession's or Connection's id. Both are copied (not retained) and
 * attached to every persisted event. */
TerminalHistory *terminal_history_create(const char *terminal_kind, uint64_t terminal_id);
void terminal_history_destroy(TerminalHistory *history);

/* persist_to_db controls only the optional database write - the in-memory
 * append (and thus terminal_history_len()/_read()) always happens. While
 * a capture window is open, "persist" means "accumulate": nothing reaches
 * the database until terminal_history_end_capture(). */
void terminal_history_append(TerminalHistory *history, const void *data, size_t len, bool persist_to_db);

/* Opens a capture window. submitted_line is the command just recorded as
 * the "input" event, kept so its echo can be matched exactly on close;
 * it is copied, not retained. Re-opening an already-open window closes
 * the previous one first, so no captured output is silently dropped. */
void terminal_history_begin_capture(TerminalHistory *history, const char *submitted_line, size_t len);

/* Writes every complete line accumulated so far, keeping the window open
 * and holding back any unterminated tail for the next call. Call after
 * each drain of terminal output: a long-running command's output lands in
 * the database as it is produced rather than only once the window closes,
 * while the prompt - which never ends in a newline - stays held back. */
void terminal_history_flush_capture(TerminalHistory *history);

/* Flushes, then closes: the held-back tail is discarded, since a window
 * ends when the shell is sat at a prompt. Writes nothing when the
 * response was empty - a command that printed nothing records no output.
 * Safe to call with no window open, so teardown paths can call it
 * unconditionally. */
void terminal_history_end_capture(TerminalHistory *history);

/* Total bytes ever appended. */
size_t terminal_history_len(const TerminalHistory *history);

/* Copies up to max_len bytes starting at offset into dst, without
 * removing anything. Returns the number of bytes actually copied (0 if
 * offset is at or past the end). */
size_t terminal_history_read(const TerminalHistory *history, size_t offset, void *dst, size_t max_len);

#endif /* WORKBENCH_TERMINAL_HISTORY_H */
