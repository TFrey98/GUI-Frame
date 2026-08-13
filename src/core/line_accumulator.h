#ifndef WORKBENCH_LINE_ACCUMULATOR_H
#define WORKBENCH_LINE_ACCUMULATOR_H

#include <stddef.h>

/*
 * Buffers arbitrary committed terminal bytes and reports one callback per
 * complete line - the "final input after Enter" capture unit, as opposed
 * to the per-keystroke bytes a terminal's commit signal actually delivers.
 * Basic line editing is applied as bytes arrive, so the reported line
 * reflects what was actually submitted, not the raw keystroke sequence:
 * Backspace/DEL erase the previously buffered byte (matching what a real
 * line-editing shell does with them) rather than being embedded literally,
 * and other C0 control bytes (Escape, Tab, Ctrl+<letter>, ...) are dropped
 * rather than appended. A line that ends up empty after editing is not
 * reported at all. Pure logic, no GTK/pty dependency, independent of any
 * terminal backend.
 */
typedef struct LineAccumulator LineAccumulator;

LineAccumulator *line_accumulator_create(void);
void line_accumulator_destroy(LineAccumulator *acc);

/* Feeds len bytes of newly committed data. Both '\n' and '\r' terminate a
 * line (the newline itself is stripped before on_line is called); a lone
 * '\r' immediately followed by '\n' reports only one line, not two. A
 * terminator on an empty (or emptied-by-backspace) line reports nothing.
 * Leftover bytes after the last terminator are kept buffered for the next
 * call. on_line's line pointer is only valid for the duration of the call. */
void line_accumulator_feed(LineAccumulator *acc, const char *data, size_t len,
                            void (*on_line)(const char *line, size_t len, void *user_data), void *user_data);

#endif /* WORKBENCH_LINE_ACCUMULATOR_H */
