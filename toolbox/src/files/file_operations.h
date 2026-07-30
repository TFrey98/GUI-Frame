#ifndef TOOLBOX_FILE_OPERATIONS_H
#define TOOLBOX_FILE_OPERATIONS_H

#include <stdbool.h>

#include "workspace_root.h"

typedef enum FileOperationResult {
    FILE_OP_OK,
    FILE_OP_ALREADY_EXISTS,
    FILE_OP_NOT_FOUND,
    FILE_OP_PERMISSION_DENIED,
    FILE_OP_OUTSIDE_WORKSPACE,
    FILE_OP_DIRECTORY_NOT_EMPTY,
    FILE_OP_INVALID_NAME,
    FILE_OP_IO_ERROR
} FileOperationResult;

/* Every function below routes relative_path (and old/new_relative_path)
 * through workspace_root_resolve_path() first - FILE_OP_OUTSIDE_WORKSPACE
 * covers both a real escape attempt and a malformed/empty path, since
 * resolve_path() itself doesn't distinguish those. Each also rejects an
 * empty, "."/".." final path component as FILE_OP_INVALID_NAME. */

FileOperationResult file_create(const WorkspaceRoot *root, const char *relative_path);
FileOperationResult directory_create(const WorkspaceRoot *root, const char *relative_path);
FileOperationResult file_rename(const WorkspaceRoot *root, const char *old_relative_path,
                                 const char *new_relative_path);

/* recursive controls behavior on a non-empty directory: false fails
 * with FILE_OP_DIRECTORY_NOT_EMPTY, true deletes it and everything
 * under it (never following a symlink into deleting its target - a
 * symlink entry is always just unlink()'d itself). */
FileOperationResult file_delete(const WorkspaceRoot *root, const char *relative_path, bool recursive);

/* Real, freshly-checked emptiness (not any in-memory FileTree state) -
 * used to decide whether Delete needs to confirm. Returns false if
 * relative_path doesn't resolve, isn't a directory, or can't be read -
 * erring toward "needs confirmation" rather than silently skipping it. */
bool file_operations_directory_is_empty(const WorkspaceRoot *root, const char *relative_path);

#endif /* TOOLBOX_FILE_OPERATIONS_H */
