/*
 * Pure unit test for AnsiStripper (src/core/ansi_strip.c) - strips ANSI/
 * VT100 escape sequences (SGR color codes, OSC title/hyperlink sequences,
 * simple two-byte escapes) from captured terminal output before it's
 * persisted to the database, including sequences split across separate
 * feed() calls (chunk boundaries). No GTK, no pty, no threads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/ansi_strip.h"

static const char *TEST_NAME = "ansi_strip_test";
static int g_failures = 0;

static void expect_feed(AnsiStripper *stripper, const char *case_name, const void *data, size_t len,
                         const char *expected) {
    unsigned char *out = NULL;
    size_t out_len = 0;
    ansi_stripper_feed(stripper, data, len, &out, &out_len);

    size_t expected_len = strlen(expected);
    int matches = (out_len == expected_len) && (expected_len == 0 || memcmp(out, expected, expected_len) == 0);
    if (!matches) {
        fprintf(stderr, "%s: [%s] expected %zu byte(s) \"%s\", got %zu byte(s) \"%.*s\"\n", TEST_NAME, case_name,
                expected_len, expected, out_len, (int)out_len, out ? (const char *)out : "");
        g_failures++;
    }
    free(out);
}

int main(void) {
    AnsiStripper *stripper = ansi_stripper_create();

    /* plain text with no escapes passes through unchanged */
    expect_feed(stripper, "plain text", "hello world", 11, "hello world");

    /* the exact pattern reported: SGR color codes around plain text */
    static const char colored[] = "\x1b[91mRE:\x1b[00m\x1b[94m is there a way\x1b[00m";
    expect_feed(stripper, "SGR color codes", colored, sizeof(colored) - 1, "RE: is there a way");

    /* an OSC sequence (e.g. setting the terminal title), BEL-terminated */
    static const char osc[] = "\x1b]0;window title\x07visible text";
    expect_feed(stripper, "OSC BEL-terminated", osc, sizeof(osc) - 1, "visible text");

    /* an OSC sequence terminated by ST (ESC \) instead of BEL */
    static const char osc_st[] = "\x1b]8;;http://example.com\x1b\\link text\x1b]8;;\x1b\\";
    expect_feed(stripper, "OSC ST-terminated", osc_st, sizeof(osc_st) - 1, "link text");

    /* a simple two-byte escape (not a CSI/OSC sequence) */
    static const char simple[] = "before\x1bMafter";
    expect_feed(stripper, "simple two-byte escape", simple, sizeof(simple) - 1, "beforeafter");

    /* a CSI sequence split exactly across two feed() calls must still be
     * fully removed, not leak a fragment as literal text */
    static const char split_a[] = "hello\x1b[9";
    static const char split_b[] = "1mworld";
    expect_feed(stripper, "CSI split across calls (part 1)", split_a, sizeof(split_a) - 1, "hello");
    expect_feed(stripper, "CSI split across calls (part 2)", split_b, sizeof(split_b) - 1, "world");

    /* reset() must discard a pending partial sequence rather than let it
     * bleed into unrelated later text */
    static const char partial[] = "abc\x1b[3";
    unsigned char *out = NULL;
    size_t out_len = 0;
    ansi_stripper_feed(stripper, partial, sizeof(partial) - 1, &out, &out_len);
    if (out_len != 3 || memcmp(out, "abc", 3) != 0) {
        fprintf(stderr, "%s: [reset setup] expected \"abc\" before the pending partial sequence\n", TEST_NAME);
        g_failures++;
    }
    free(out);
    ansi_stripper_reset(stripper);
    expect_feed(stripper, "reset discards pending partial sequence", "xyz", 3, "xyz");

    ansi_stripper_destroy(stripper);

    if (g_failures == 0) {
        printf("%s: plain passthrough, SGR color codes, OSC (BEL and ST terminated), simple escapes, "
               "cross-call split sequences, and reset all verified\n",
               TEST_NAME);
        return 0;
    }
    return 1;
}
