#ifndef WORKBENCH_APP_PATHS_H
#define WORKBENCH_APP_PATHS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Where this installation keeps its writable state: the files/ workspace
 * root, the tools/ root, and workbench.db.
 *
 * Resolution order, decided once per process and cached:
 *
 *   1. <exe_dir>, whenever that directory is writable. This is the
 *      development and test layout - build/workbench keeps its data in
 *      build/, and the GTK smoke tests in build/tests/ keep resolving the
 *      same exe-relative roots they always have.
 *   2. $XDG_DATA_HOME/workbench (or $HOME/.local/share/workbench),
 *      created if missing. This is the installed layout: a packaged
 *      binary lives in /usr/bin, which is read-only for the user running
 *      it, so per-user state has to live in the home directory instead.
 *
 * Returns NULL only if neither location can be resolved or created;
 * callers that treat the workspace root as fatal to startup already
 * handle that as a failure.
 */
const char *app_paths_data_dir(void);

/* Writes <data_dir>/name into out and creates it (mode 0755) if missing.
 * Returns false if the data dir is unavailable, the path would not fit,
 * or the directory can't be created. */
bool app_paths_data_subdir(const char *name, char *out, size_t out_size);

/* Writes <data_dir>/name into out without creating anything - for files
 * whose owner (sqlite, an exporter) creates them on first write. */
bool app_paths_data_file(const char *name, char *out, size_t out_size);

/*
 * One-time migration for a data subdirectory that has been renamed
 * between releases - call before app_paths_data_subdir() creates the new
 * one, so an existing install keeps its contents.
 *
 * Renames <data_dir>/legacy_name to <data_dir>/name when legacy_name
 * exists and name either doesn't, or exists but is empty (the case where
 * something already created the new directory before this ran; rmdir()
 * refuses a non-empty one, which is exactly the safety check wanted).
 * A name that already holds real content is left completely alone.
 *
 * Returns true only when a rename actually happened. Nothing here is
 * fatal - having no legacy directory is the normal case on a fresh
 * install.
 */
bool app_paths_rename_legacy_subdir(const char *legacy_name, const char *name);

#endif /* WORKBENCH_APP_PATHS_H */
