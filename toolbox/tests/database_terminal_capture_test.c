/*
 * Exercises the terminal_events schema/capture API added to db/database.c:
 * database_init_schema(), database_record_terminal_event() (the only
 * prepared-statement INSERT in the project - BLOB-bound so embedded NULs
 * and invalid UTF-8 survive byte-exact), database_for_each_terminal_event(),
 * and database_clear_terminal_events(). Real mkdtemp()'d sqlite file, no
 * GTK.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db/database.h"

static const char *TEST_NAME = "database_terminal_capture_test";

typedef struct CapturedRow {
    uint64_t id;
    char terminal_kind[32];
    uint64_t terminal_id;
    char direction[32];
    unsigned char data[64];
    size_t data_len;
} CapturedRow;

#define MAX_ROWS 8
static CapturedRow g_rows[MAX_ROWS];
static int g_row_count;

static void collect_row(uint64_t id, const char *terminal_kind, uint64_t terminal_id, const char *direction,
                         int64_t captured_at, const void *data, size_t data_len, void *user_data) {
    (void)user_data;
    (void)captured_at;
    if (g_row_count >= MAX_ROWS || data_len > sizeof(g_rows[0].data)) {
        return;
    }
    CapturedRow *row = &g_rows[g_row_count++];
    row->id = id;
    strncpy(row->terminal_kind, terminal_kind, sizeof(row->terminal_kind) - 1);
    row->terminal_id = terminal_id;
    strncpy(row->direction, direction, sizeof(row->direction) - 1);
    memcpy(row->data, data, data_len);
    row->data_len = data_len;
}

int main(void) {
    int status = 0;

    char dir_template[] = "/tmp/db_capture_test_XXXXXX";
    char *dir = mkdtemp(dir_template);
    if (!dir) {
        fprintf(stderr, "%s: mkdtemp failed\n", TEST_NAME);
        return 1;
    }
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/toolbox.db", dir);

    if (database_open(db_path) != 0) {
        fprintf(stderr, "%s: database_open failed\n", TEST_NAME);
        return 1;
    }
    if (database_init_schema() != 0) {
        fprintf(stderr, "%s: database_init_schema failed\n", TEST_NAME);
        status = 1;
    }

    static const char command[] = "ls -la";
    if (database_record_terminal_event("local", 1, "input", command, strlen(command)) != 0) {
        fprintf(stderr, "%s: recording the input event failed\n", TEST_NAME);
        status = 1;
    }

    /* Embedded NUL + an invalid UTF-8 leading byte (0xFF) - must round-trip
     * byte-exact through the BLOB column, not be truncated or mangled. */
    static const unsigned char binary_output[] = {'o', 'k', '\0', 0xFF, 'x', '\n'};
    if (database_record_terminal_event("connection", 2, "output", binary_output, sizeof(binary_output)) != 0) {
        fprintf(stderr, "%s: recording the output event failed\n", TEST_NAME);
        status = 1;
    }

    g_row_count = 0;
    int visited = database_for_each_terminal_event(collect_row, NULL);
    if (visited != 2 || g_row_count != 2) {
        fprintf(stderr, "%s: expected 2 rows, database_for_each_terminal_event visited %d\n", TEST_NAME, visited);
        status = 1;
    } else {
        if (g_rows[0].id >= g_rows[1].id) {
            fprintf(stderr, "%s: expected rows ordered oldest-first by id\n", TEST_NAME);
            status = 1;
        }
        if (strcmp(g_rows[0].terminal_kind, "local") != 0 || g_rows[0].terminal_id != 1 ||
            strcmp(g_rows[0].direction, "input") != 0 || g_rows[0].data_len != strlen(command) ||
            memcmp(g_rows[0].data, command, strlen(command)) != 0) {
            fprintf(stderr, "%s: input row fields did not round-trip correctly\n", TEST_NAME);
            status = 1;
        }
        if (strcmp(g_rows[1].terminal_kind, "connection") != 0 || g_rows[1].terminal_id != 2 ||
            strcmp(g_rows[1].direction, "output") != 0 || g_rows[1].data_len != sizeof(binary_output) ||
            memcmp(g_rows[1].data, binary_output, sizeof(binary_output)) != 0) {
            fprintf(stderr, "%s: output row's binary data did not round-trip byte-exact\n", TEST_NAME);
            status = 1;
        }
    }

    /* A single-character "input" event (e.g. a real "q" or "l" command) is
     * a legitimate row on its own - database_record_terminal_event() itself
     * applies no length-based filtering (that's handled one layer up, by
     * TerminalHistory's persist_to_db gate - see terminal_history_capture_test.c). */
    if (database_record_terminal_event("local", 1, "input", "q", 1) != 0) {
        fprintf(stderr, "%s: recording a single-character input event failed\n", TEST_NAME);
        status = 1;
    }
    g_row_count = 0;
    visited = database_for_each_terminal_event(collect_row, NULL);
    if (visited != 3 || g_row_count != 3) {
        fprintf(stderr, "%s: expected 3 rows after the single-character input event, got %d\n", TEST_NAME, visited);
        status = 1;
    } else {
        const CapturedRow *last = &g_rows[2];
        if (strcmp(last->direction, "input") != 0 || last->data_len != 1 || last->data[0] != 'q') {
            fprintf(stderr, "%s: expected the surviving single-character row to be the \"q\" input event\n",
                    TEST_NAME);
            status = 1;
        }
    }

    if (database_clear_terminal_events() != 0) {
        fprintf(stderr, "%s: database_clear_terminal_events failed\n", TEST_NAME);
        status = 1;
    }
    g_row_count = 0;
    visited = database_for_each_terminal_event(collect_row, NULL);
    if (visited != 0 || g_row_count != 0) {
        fprintf(stderr, "%s: expected 0 rows after clearing, got %d\n", TEST_NAME, visited);
        status = 1;
    }

    /* Clearing must be a true blank slate, not just an emptied table -
     * the next inserted row should get id 1 again, not continue climbing
     * from wherever AUTOINCREMENT last left off (which a plain DELETE
     * alone would do). */
    if (database_record_terminal_event("local", 1, "input", "fresh", 5) != 0) {
        fprintf(stderr, "%s: recording an event after clearing failed\n", TEST_NAME);
        status = 1;
    }
    g_row_count = 0;
    visited = database_for_each_terminal_event(collect_row, NULL);
    if (visited != 1 || g_row_count != 1) {
        fprintf(stderr, "%s: expected exactly 1 row after clearing and inserting once, got %d\n", TEST_NAME,
                visited);
        status = 1;
    } else if (g_rows[0].id != 1) {
        fprintf(stderr, "%s: expected the first row after a clear to have id 1, got id %llu\n", TEST_NAME,
                (unsigned long long)g_rows[0].id);
        status = 1;
    }
    if (database_clear_terminal_events() != 0) {
        fprintf(stderr, "%s: second database_clear_terminal_events failed\n", TEST_NAME);
        status = 1;
    }

    database_close();
    unlink(db_path);
    rmdir(dir);

    if (status == 0) {
        printf("%s: schema init, insert, byte-exact binary round-trip, and clear-resets-id-sequence all "
               "verified\n",
               TEST_NAME);
    }
    return status;
}
