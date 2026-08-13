/*
 * Exercises TerminalHistory's persist_to_db gate (src/listeners/terminal_history.c) -
 * the mechanism that keeps a fast typist's own echoed input out of the
 * database regardless of how many bytes land in one append: the in-memory
 * buffer always grows, but a database row is only written when the caller
 * says the output-capture window is open. Real mkdtemp()'d sqlite file, no
 * GTK.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db/database.h"
#include "listeners/terminal_history.h"

static const char *TEST_NAME = "terminal_history_capture_test";

typedef struct CapturedRow {
    char direction[32];
    unsigned char data[64];
    size_t data_len;
} CapturedRow;

#define MAX_ROWS 8
static CapturedRow g_rows[MAX_ROWS];
static int g_row_count;

static void collect_row(uint64_t id, const char *terminal_kind, uint64_t terminal_id, const char *direction,
                         int64_t captured_at, const void *data, size_t data_len, void *user_data) {
    (void)id;
    (void)terminal_kind;
    (void)terminal_id;
    (void)captured_at;
    (void)user_data;
    if (g_row_count >= MAX_ROWS || data_len > sizeof(g_rows[0].data)) {
        return;
    }
    CapturedRow *row = &g_rows[g_row_count++];
    strncpy(row->direction, direction, sizeof(row->direction) - 1);
    memcpy(row->data, data, data_len);
    row->data_len = data_len;
}

static int count_rows(void) {
    g_row_count = 0;
    return database_for_each_terminal_event(collect_row, NULL);
}

int main(void) {
    int status = 0;

    char dir_template[] = "/tmp/th_capture_test_XXXXXX";
    char *dir = mkdtemp(dir_template);
    if (!dir) {
        fprintf(stderr, "%s: mkdtemp failed\n", TEST_NAME);
        return 1;
    }
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/workbench.db", dir);

    if (database_open(db_path) != 0 || database_init_schema() != 0) {
        fprintf(stderr, "%s: database setup failed\n", TEST_NAME);
        return 1;
    }

    TerminalHistory *history = terminal_history_create("local", 42);

    /* Simulates a fast typist's echo arriving as one multi-character chunk
     * (exactly the scenario a simple "skip if <=1 byte" filter would have
     * missed) while the output-capture window is closed - must grow the
     * in-memory buffer but never reach the database. */
    static const char echoed_keystrokes[] = "echo hello world";
    terminal_history_append(history, echoed_keystrokes, strlen(echoed_keystrokes), false);
    if (terminal_history_len(history) != strlen(echoed_keystrokes)) {
        fprintf(stderr, "%s: in-memory history must grow even when persist_to_db is false\n", TEST_NAME);
        status = 1;
    }
    if (count_rows() != 0) {
        fprintf(stderr, "%s: expected 0 database rows while the capture window is closed, got %d\n", TEST_NAME,
                g_row_count);
        status = 1;
    }

    /* Simulates the command's real response arriving after Enter, once the
     * capture window is open - must reach the database. */
    static const char response[] = "hello world\n";
    terminal_history_append(history, response, strlen(response), true);
    if (count_rows() != 1) {
        fprintf(stderr, "%s: expected 1 database row once the capture window is open, got %d\n", TEST_NAME,
                g_row_count);
        status = 1;
    } else if (strcmp(g_rows[0].direction, "output") != 0 || g_rows[0].data_len != strlen(response) ||
               memcmp(g_rows[0].data, response, strlen(response)) != 0) {
        fprintf(stderr, "%s: captured output row did not match the response bytes\n", TEST_NAME);
        status = 1;
    }

    /* A colored response (SGR escape codes, as e.g. grep --color or a
     * shell prompt would emit) must reach the database with the escape
     * codes stripped - readable plain text - while the in-memory buffer
     * (used for on-screen display, which needs the real codes to render
     * color) keeps the raw bytes untouched. */
    static const char colored_response[] = "\x1b[91merror:\x1b[00m something failed\n";
    static const char expected_stripped[] = "error: something failed\n";
    terminal_history_append(history, colored_response, strlen(colored_response), true);
    if (count_rows() != 2) {
        fprintf(stderr, "%s: expected 2 database rows after the colored response, got %d\n", TEST_NAME,
                g_row_count);
        status = 1;
    } else if (g_rows[1].data_len != strlen(expected_stripped) ||
               memcmp(g_rows[1].data, expected_stripped, strlen(expected_stripped)) != 0) {
        fprintf(stderr, "%s: expected the database row to have ANSI escape codes stripped to \"%s\", got \"%.*s\"\n",
                TEST_NAME, expected_stripped, (int)g_rows[1].data_len, g_rows[1].data);
        status = 1;
    }

    /* The in-memory buffer must contain every appended byte back to back,
     * raw and untouched (including the ANSI codes above) - replay/display
     * never depends on persist_to_db or on what got stripped for the
     * database. */
    size_t total_len = strlen(echoed_keystrokes) + strlen(response) + strlen(colored_response);
    if (terminal_history_len(history) != total_len) {
        fprintf(stderr, "%s: in-memory history should contain every appended byte regardless of persistence\n",
                TEST_NAME);
        status = 1;
    } else {
        char replayed[256];
        size_t n = terminal_history_read(history, terminal_history_len(history) - strlen(colored_response), replayed,
                                          sizeof(replayed));
        if (n != strlen(colored_response) || memcmp(replayed, colored_response, n) != 0) {
            fprintf(stderr, "%s: in-memory replay of the colored response should still contain the raw ANSI "
                            "escape codes, not the stripped text\n",
                    TEST_NAME);
            status = 1;
        }
    }

    terminal_history_destroy(history);
    database_close();
    unlink(db_path);
    rmdir(dir);

    if (status == 0) {
        printf("%s: in-memory append always happens, database persistence only when the capture window is "
               "open, and ANSI escape codes are stripped for the database while staying intact in-memory for "
               "display, all verified\n",
               TEST_NAME);
    }
    return status;
}
