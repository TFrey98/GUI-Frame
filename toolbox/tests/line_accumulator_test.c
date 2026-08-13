/*
 * Pure unit test for LineAccumulator (src/core/line_accumulator.c) - the
 * "final input after Enter" capture unit shared by the local-terminal and
 * connection-terminal commit handlers. No GTK, no pty, no threads.
 */
#include <stdio.h>
#include <string.h>

#include "core/line_accumulator.h"

static const char *TEST_NAME = "line_accumulator_test";
static int g_failures = 0;

#define MAX_LINES 16
static char g_lines[MAX_LINES][256];
static int g_line_count;

static void reset_capture(void) {
    g_line_count = 0;
}

static void capture_line(const char *line, size_t len, void *user_data) {
    (void)user_data;
    if (g_line_count >= MAX_LINES || len >= sizeof(g_lines[0])) {
        return;
    }
    memcpy(g_lines[g_line_count], line, len);
    g_lines[g_line_count][len] = '\0';
    g_line_count++;
}

static void expect_lines(const char *case_name, int expected_count, const char *const *expected) {
    if (g_line_count != expected_count) {
        fprintf(stderr, "%s: [%s] expected %d line(s), got %d\n", TEST_NAME, case_name, expected_count,
                g_line_count);
        g_failures++;
        return;
    }
    for (int i = 0; i < expected_count; i++) {
        if (strcmp(g_lines[i], expected[i]) != 0) {
            fprintf(stderr, "%s: [%s] line %d: expected \"%s\", got \"%s\"\n", TEST_NAME, case_name, i, expected[i],
                    g_lines[i]);
            g_failures++;
        }
    }
}

int main(void) {
    LineAccumulator *acc = line_accumulator_create();

    /* one line, one feed() call */
    reset_capture();
    line_accumulator_feed(acc, "hello\n", 6, capture_line, NULL);
    expect_lines("single line", 1, (const char *[]){"hello"});

    /* a line split across two feed() calls stays one line */
    reset_capture();
    line_accumulator_feed(acc, "hel", 3, capture_line, NULL);
    if (g_line_count != 0) {
        fprintf(stderr, "%s: [split line] partial data fired a line before the terminator arrived\n", TEST_NAME);
        g_failures++;
    }
    line_accumulator_feed(acc, "lo\n", 3, capture_line, NULL);
    expect_lines("split line", 1, (const char *[]){"hello"});

    /* several lines in a single feed() call */
    reset_capture();
    line_accumulator_feed(acc, "a\nb\nc\n", 6, capture_line, NULL);
    expect_lines("multiple lines", 3, (const char *[]){"a", "b", "c"});

    /* CRLF terminators - one line each, not an extra empty line between them */
    reset_capture();
    line_accumulator_feed(acc, "line1\r\nline2\r\n", 14, capture_line, NULL);
    expect_lines("crlf", 2, (const char *[]){"line1", "line2"});

    /* CRLF split exactly between the \r and the \n across two feed() calls */
    reset_capture();
    line_accumulator_feed(acc, "line1\r", 6, capture_line, NULL);
    line_accumulator_feed(acc, "\nline2\n", 7, capture_line, NULL);
    expect_lines("crlf split across feed calls", 2, (const char *[]){"line1", "line2"});

    /* a lone \r (old Mac style) still terminates a line on its own */
    reset_capture();
    line_accumulator_feed(acc, "abc\rdef\n", 8, capture_line, NULL);
    expect_lines("lone cr", 2, (const char *[]){"abc", "def"});

    /* trailing partial data with no terminator never fires, and is
     * prepended to whatever completes it later */
    reset_capture();
    line_accumulator_feed(acc, "partial", 7, capture_line, NULL);
    if (g_line_count != 0) {
        fprintf(stderr, "%s: [trailing partial] unterminated data fired a line\n", TEST_NAME);
        g_failures++;
    }
    line_accumulator_feed(acc, "-rest\n", 6, capture_line, NULL);
    expect_lines("trailing partial completed later", 1, (const char *[]){"partial-rest"});

    /* Backspace erases the previously buffered byte instead of being
     * embedded literally - "hello" then 3 backspaces (removing "llo")
     * leaves "he". */
    reset_capture();
    line_accumulator_feed(acc, "hello\x08\x08\x08\n", 9, capture_line, NULL);
    expect_lines("backspace erases", 1, (const char *[]){"he"});

    /* DEL (0x7f) behaves the same as backspace (0x08) */
    reset_capture();
    line_accumulator_feed(acc, "abcd\x7f\n", 6, capture_line, NULL);
    expect_lines("DEL erases", 1, (const char *[]){"abc"});

    /* Backspace on an empty buffer is a no-op, not an error/underflow.
     * Split into two adjacent string literals so \x08's greedy hex-digit
     * matching doesn't swallow the following 'a' as part of the escape. */
    reset_capture();
    line_accumulator_feed(acc, "\x08\x08"
                                "abc\n",
                           6, capture_line, NULL);
    expect_lines("backspace on empty buffer is a no-op", 1, (const char *[]){"abc"});

    /* Other C0 control bytes (Escape, Tab, ...) are dropped, not embedded
     * and not treated as an erase */
    reset_capture();
    line_accumulator_feed(acc, "ab\x1b\tcd\n", 7, capture_line, NULL);
    expect_lines("control bytes dropped", 1, (const char *[]){"abcd"});

    /* The exact garbage pattern this fix targets: a run of backspaces (all
     * no-ops on an empty buffer), one real character, then a backspace
     * erasing it - ends up empty, so nothing should be reported at all,
     * not an empty-string line. */
    reset_capture();
    line_accumulator_feed(acc, "\x08\x08\x08\x08-\x08\n", 7, capture_line, NULL);
    if (g_line_count != 0) {
        fprintf(stderr, "%s: [fully backspaced line] expected no callback for a line emptied by backspace\n",
                TEST_NAME);
        g_failures++;
    }

    /* Plain Enter with nothing typed also reports nothing */
    reset_capture();
    line_accumulator_feed(acc, "\n", 1, capture_line, NULL);
    if (g_line_count != 0) {
        fprintf(stderr, "%s: [empty enter] expected no callback for an empty line\n", TEST_NAME);
        g_failures++;
    }

    line_accumulator_destroy(acc);

    if (g_failures == 0) {
        printf("%s: single/split/multi-line, CRLF (including split), lone CR, partial-buffering, "
               "backspace/DEL erasing, control-byte dropping, and empty-line suppression all verified\n",
               TEST_NAME);
        return 0;
    }
    return 1;
}
