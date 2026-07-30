#include "file_operations.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *leaf_name(const char *relative_path) {
    const char *slash = strrchr(relative_path, '/');
    return slash ? slash + 1 : relative_path;
}

static bool leaf_name_is_valid(const char *relative_path) {
    const char *leaf = leaf_name(relative_path);
    if (leaf[0] == '\0') {
        return false;
    }
    return strcmp(leaf, ".") != 0 && strcmp(leaf, "..") != 0;
}

static FileOperationResult result_from_errno(int err) {
    switch (err) {
        case EEXIST:
            return FILE_OP_ALREADY_EXISTS;
        case ENOENT:
            return FILE_OP_NOT_FOUND;
        case EACCES:
        case EPERM:
            return FILE_OP_PERMISSION_DENIED;
        case ENOTEMPTY:
            return FILE_OP_DIRECTORY_NOT_EMPTY;
        default:
            return FILE_OP_IO_ERROR;
    }
}

FileOperationResult file_create(const WorkspaceRoot *root, const char *relative_path) {
    if (!leaf_name_is_valid(relative_path)) {
        return FILE_OP_INVALID_NAME;
    }
    char resolved[4096];
    if (!workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
        return FILE_OP_OUTSIDE_WORKSPACE;
    }
    int fd = open(resolved, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        return result_from_errno(errno);
    }
    close(fd);
    return FILE_OP_OK;
}

FileOperationResult directory_create(const WorkspaceRoot *root, const char *relative_path) {
    if (!leaf_name_is_valid(relative_path)) {
        return FILE_OP_INVALID_NAME;
    }
    char resolved[4096];
    if (!workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
        return FILE_OP_OUTSIDE_WORKSPACE;
    }
    if (mkdir(resolved, 0755) != 0) {
        return result_from_errno(errno);
    }
    return FILE_OP_OK;
}

FileOperationResult file_rename(const WorkspaceRoot *root, const char *old_relative_path,
                                 const char *new_relative_path) {
    if (!leaf_name_is_valid(new_relative_path)) {
        return FILE_OP_INVALID_NAME;
    }

    char old_resolved[4096];
    if (!workspace_root_resolve_path(root, old_relative_path, old_resolved, sizeof(old_resolved))) {
        return FILE_OP_OUTSIDE_WORKSPACE;
    }
    char new_resolved[4096];
    if (!workspace_root_resolve_path(root, new_relative_path, new_resolved, sizeof(new_resolved))) {
        return FILE_OP_OUTSIDE_WORKSPACE;
    }

    /* rename() itself would silently overwrite an existing destination -
     * check first (accepted as non-atomic; not a realistic race for a
     * single-user local interactive tool). */
    struct stat st;
    if (lstat(new_resolved, &st) == 0) {
        return FILE_OP_ALREADY_EXISTS;
    }
    if (lstat(old_resolved, &st) != 0) {
        return result_from_errno(errno);
    }

    if (rename(old_resolved, new_resolved) != 0) {
        return result_from_errno(errno);
    }
    return FILE_OP_OK;
}

/* Recursively removes absolute_path and everything under it. A symlink
 * entry is always just unlink()'d itself (via lstat's own type check),
 * never followed into deleting whatever it points at. */
static FileOperationResult remove_recursive(const char *absolute_path) {
    struct stat st;
    if (lstat(absolute_path, &st) != 0) {
        return result_from_errno(errno);
    }

    if (!S_ISDIR(st.st_mode)) {
        if (unlink(absolute_path) != 0) {
            return result_from_errno(errno);
        }
        return FILE_OP_OK;
    }

    DIR *dir = opendir(absolute_path);
    if (!dir) {
        return result_from_errno(errno);
    }

    FileOperationResult result = FILE_OP_OK;
    struct dirent *entry;
    while (result == FILE_OP_OK && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child_path[4400];
        snprintf(child_path, sizeof(child_path), "%s/%s", absolute_path, entry->d_name);
        result = remove_recursive(child_path);
    }
    closedir(dir);

    if (result != FILE_OP_OK) {
        return result;
    }
    if (rmdir(absolute_path) != 0) {
        return result_from_errno(errno);
    }
    return FILE_OP_OK;
}

FileOperationResult file_delete(const WorkspaceRoot *root, const char *relative_path, bool recursive) {
    char resolved[4096];
    if (!workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
        return FILE_OP_OUTSIDE_WORKSPACE;
    }

    struct stat st;
    if (lstat(resolved, &st) != 0) {
        return result_from_errno(errno);
    }

    if (!S_ISDIR(st.st_mode)) {
        if (unlink(resolved) != 0) {
            return result_from_errno(errno);
        }
        return FILE_OP_OK;
    }

    if (!recursive) {
        if (rmdir(resolved) != 0) {
            return result_from_errno(errno);
        }
        return FILE_OP_OK;
    }

    return remove_recursive(resolved);
}

bool file_operations_directory_is_empty(const WorkspaceRoot *root, const char *relative_path) {
    char resolved[4096];
    if (!workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
        return false;
    }

    DIR *dir = opendir(resolved);
    if (!dir) {
        return false;
    }

    bool empty = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        empty = false;
        break;
    }
    closedir(dir);
    return empty;
}
