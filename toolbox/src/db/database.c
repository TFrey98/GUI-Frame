#include "database.h"

#include <sqlite3.h>
#include <stddef.h>
#include <time.h>

/* Single global connection - the app only ever needs one database and
 * never touches it from more than the main thread, so there's no
 * connection pool or handle threading through callers. */
static sqlite3 *g_db = NULL;

int database_open(const char *path) {
    if (sqlite3_open(path, &g_db) != SQLITE_OK) {
        /* sqlite3_open() can still allocate a handle even on failure -
         * close it before dropping the reference so the "g_db == NULL
         * means no usable database" invariant holds without leaking. */
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }
    return 0;
}

void database_close(void) {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

int database_exec(const char *sql) {
    if (!g_db) {
        return -1;
    }
    char *err = NULL;
    /* No row callback (NULL, NULL): only for statements that don't return
     * rows (CREATE/INSERT/UPDATE/... ) - a SELECT's results would be
     * silently discarded. */
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int database_init_schema(void) {
    return database_exec("CREATE TABLE IF NOT EXISTS terminal_events ("
                          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                          "terminal_kind TEXT NOT NULL,"
                          "terminal_id INTEGER NOT NULL,"
                          "direction TEXT NOT NULL,"
                          "captured_at INTEGER NOT NULL,"
                          "data BLOB NOT NULL)");
}

/* The only two functions in this project that go through
 * sqlite3_prepare_v2()/bind/step/finalize rather than database_exec() -
 * captured terminal bytes can contain embedded NULs, quotes, or invalid
 * UTF-8, so binding them as a parameter (never formatting them into a SQL
 * string) is the only safe way to store them. */
int database_record_terminal_event(const char *terminal_kind, uint64_t terminal_id, const char *direction,
                                    const void *data, size_t len) {
    if (!g_db) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    static const char sql[] = "INSERT INTO terminal_events (terminal_kind, terminal_id, direction, captured_at, "
                               "data) VALUES (?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, terminal_kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)terminal_id);
    sqlite3_bind_text(stmt, 3, direction, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)time(NULL));
    sqlite3_bind_blob(stmt, 5, data, (int)len, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int database_for_each_terminal_event(TerminalEventCallback cb, void *user_data) {
    if (!g_db) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    static const char sql[] = "SELECT id, terminal_kind, terminal_id, direction, captured_at, data "
                               "FROM terminal_events ORDER BY id ASC";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    int count = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        uint64_t id = (uint64_t)sqlite3_column_int64(stmt, 0);
        const char *terminal_kind = (const char *)sqlite3_column_text(stmt, 1);
        uint64_t terminal_id = (uint64_t)sqlite3_column_int64(stmt, 2);
        const char *direction = (const char *)sqlite3_column_text(stmt, 3);
        int64_t captured_at = (int64_t)sqlite3_column_int64(stmt, 4);
        const void *data = sqlite3_column_blob(stmt, 5);
        size_t data_len = (size_t)sqlite3_column_bytes(stmt, 5);
        cb(id, terminal_kind, terminal_id, direction, captured_at, data, data_len, user_data);
        count++;
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? count : -1;
}

int database_clear_terminal_events(void) {
    /* Drop and rebuild rather than DELETE - a true blank slate: DELETE
     * alone leaves the AUTOINCREMENT counter (sqlite_sequence) wherever it
     * was, so ids just keep climbing across repeated clears instead of
     * starting back at 1. */
    if (database_exec("DROP TABLE IF EXISTS terminal_events") != 0) {
        return -1;
    }
    return database_init_schema();
}
