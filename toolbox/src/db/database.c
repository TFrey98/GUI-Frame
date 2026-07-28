#include "database.h"

#include <sqlite3.h>
#include <stddef.h>

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
