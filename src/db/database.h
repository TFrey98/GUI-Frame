#ifndef WORKBENCH_DATABASE_H
#define WORKBENCH_DATABASE_H

#include <stddef.h>
#include <stdint.h>

/* Minimal process-global persistence seam. The current application treats
 * database availability as optional and uses one main-thread connection. */

/* Opens (and creates, if needed) the SQLite database at path. Returns 0 on success. */
int database_open(const char *path);
/* Idempotent; safe when no database is open. */
void database_close(void);

/* Executes a statement with no result set. Returns 0 on success. */
int database_exec(const char *sql);

/* Creates the terminal_events table if it doesn't already exist. Call once
 * after a successful database_open(). Returns 0 on success (including when
 * no database is open - nothing to do). */
int database_init_schema(void);

/* Records one captured terminal input/output event. terminal_kind is
 * "local" or "connection"; direction is "input" or "output"; data is
 * arbitrary bytes (may contain embedded NULs or invalid UTF-8) bound as a
 * BLOB, never formatted into SQL text. No-op (returns -1) if no database
 * is open. */
int database_record_terminal_event(const char *terminal_kind, uint64_t terminal_id, const char *direction,
                                    const void *data, size_t len);

/* Reads every captured event, oldest first, invoking cb once per row.
 * data/data_len point at the row's raw bytes for the duration of the
 * callback only. Returns the number of rows visited, or -1 if no database
 * is open or the query failed. */
typedef void (*TerminalEventCallback)(uint64_t id, const char *terminal_kind, uint64_t terminal_id,
                                       const char *direction, int64_t captured_at, const void *data, size_t data_len,
                                       void *user_data);
int database_for_each_terminal_event(TerminalEventCallback cb, void *user_data);

/* Deletes every captured event. Returns 0 on success. */
int database_clear_terminal_events(void);

#endif /* WORKBENCH_DATABASE_H */
