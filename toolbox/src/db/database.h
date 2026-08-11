#ifndef TOOLBOX_DATABASE_H
#define TOOLBOX_DATABASE_H

/* Minimal process-global persistence seam. The current application treats
 * database availability as optional and uses one main-thread connection. */

/* Opens (and creates, if needed) the SQLite database at path. Returns 0 on success. */
int database_open(const char *path);
/* Idempotent; safe when no database is open. */
void database_close(void);

/* Executes a statement with no result set. Returns 0 on success. */
int database_exec(const char *sql);

#endif /* TOOLBOX_DATABASE_H */
