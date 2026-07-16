#ifndef TOOLBOX_DATABASE_H
#define TOOLBOX_DATABASE_H

/* Opens (and creates, if needed) the SQLite database at path. Returns 0 on success. */
int database_open(const char *path);
void database_close(void);

/* Executes a statement with no result set. Returns 0 on success. */
int database_exec(const char *sql);

#endif /* TOOLBOX_DATABASE_H */
