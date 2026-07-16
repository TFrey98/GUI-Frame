#include "database.h"

#include <sqlite3.h>
#include <stddef.h>

static sqlite3 *g_db = NULL;

int database_open(const char *path) {
    if (sqlite3_open(path, &g_db) != SQLITE_OK) {
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
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        return -1;
    }
    return 0;
}
