#ifndef WORKBENCH_WORKSPACE_ROOT_H
#define WORKBENCH_WORKSPACE_ROOT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct WorkspaceRoot {
    char display_path[4096];
    char canonical_path[4096];
} WorkspaceRoot;

/* Resolves <exe_dir>/workbench, creating it if missing. Returns false
 * (leaving *out unwritten) if it can't be created or accessed - callers
 * must treat that as fatal to startup, not a soft-fail. */
bool workspace_root_init(WorkspaceRoot *out);

/* Same population as workspace_root_init(), for a directory that
 * already exists and is already resolved (e.g. toolkit_index_dir()) -
 * no /proc/self/exe re-derivation, no mkdir(). Returns false (leaving
 * *out unwritten) if absolute_directory can't be realpath()'d. */
bool workspace_root_init_at(WorkspaceRoot *out, const char *absolute_directory);

/*
 * The containment function every filesystem operation in this subsystem
 * must go through. relative_path must have no leading '/' and no ".."
 * component; on success writes the fully resolved, symlink-free
 * absolute path into resolved_path and returns true.
 *
 * Works whether or not the final path component exists yet (so a
 * future file_create() can resolve a not-yet-existing target), but
 * every existing ancestor directory is canonicalized via realpath(),
 * so a symlink anywhere in the path that escapes the root is caught.
 */
bool workspace_root_resolve_path(const WorkspaceRoot *root, const char *relative_path, char *resolved_path,
                                  size_t resolved_size);

#endif /* WORKBENCH_WORKSPACE_ROOT_H */
