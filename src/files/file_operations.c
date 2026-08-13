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
    return file_move(root, old_relative_path, root, new_relative_path);
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

/* Streams src_absolute's bytes into a newly-created dest_absolute
 * (O_EXCL - never overwrites), chmod'd to mode. Any failure along the
 * way unlinks whatever partial file was created and leaves the source
 * untouched. */
static FileOperationResult copy_regular_file(const char *src_absolute, const char *dest_absolute, mode_t mode) {
    FILE *in = fopen(src_absolute, "rb");
    if (!in) {
        return result_from_errno(errno);
    }
    int fd = open(dest_absolute, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        fclose(in);
        return result_from_errno(errno);
    }
    FILE *out = fdopen(fd, "wb");
    if (!out) {
        close(fd);
        fclose(in);
        unlink(dest_absolute);
        return FILE_OP_IO_ERROR;
    }

    char buf[65536];
    size_t n;
    bool ok = true;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
        ok = fwrite(buf, 1, n, out) == n;
    }
    ok = ok && !ferror(in);
    fclose(in);
    bool flushed = fflush(out) == 0;
    fclose(out);
    if (!ok || !flushed) {
        unlink(dest_absolute);
        return FILE_OP_IO_ERROR;
    }
    chmod(dest_absolute, mode);
    return FILE_OP_OK;
}

/* Recursively copies src_absolute to dest_absolute (both already-
 * resolved, contained absolute paths). A symlink entry is recreated via
 * readlink()+symlink() - its target is never opened/followed, same
 * discipline remove_recursive already has for delete. */
static FileOperationResult copy_recursive(const char *src_absolute, const char *dest_absolute) {
    struct stat st;
    if (lstat(src_absolute, &st) != 0) {
        return result_from_errno(errno);
    }

    if (S_ISLNK(st.st_mode)) {
        char target[4096];
        ssize_t len = readlink(src_absolute, target, sizeof(target) - 1);
        if (len < 0) {
            return result_from_errno(errno);
        }
        target[len] = '\0';
        if (symlink(target, dest_absolute) != 0) {
            return result_from_errno(errno);
        }
        return FILE_OP_OK;
    }

    if (!S_ISDIR(st.st_mode)) {
        return copy_regular_file(src_absolute, dest_absolute, st.st_mode & 0777);
    }

    if (mkdir(dest_absolute, st.st_mode & 0777) != 0) {
        return result_from_errno(errno);
    }

    DIR *dir = opendir(src_absolute);
    if (!dir) {
        return result_from_errno(errno);
    }

    FileOperationResult result = FILE_OP_OK;
    struct dirent *entry;
    while (result == FILE_OP_OK && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child_src[4400], child_dest[4400];
        snprintf(child_src, sizeof(child_src), "%s/%s", src_absolute, entry->d_name);
        snprintf(child_dest, sizeof(child_dest), "%s/%s", dest_absolute, entry->d_name);
        result = copy_recursive(child_src, child_dest);
    }
    closedir(dir);
    return result;
}

FileOperationResult file_copy(const WorkspaceRoot *src_root, const char *src_relative_path,
                               const WorkspaceRoot *dest_root, const char *dest_relative_path) {
    if (!leaf_name_is_valid(dest_relative_path)) {
        return FILE_OP_INVALID_NAME;
    }
    char src_resolved[4096];
    if (!workspace_root_resolve_path(src_root, src_relative_path, src_resolved, sizeof(src_resolved))) {
        return FILE_OP_OUTSIDE_WORKSPACE;
    }
    char dest_resolved[4096];
    if (!workspace_root_resolve_path(dest_root, dest_relative_path, dest_resolved, sizeof(dest_resolved))) {
        return FILE_OP_OUTSIDE_WORKSPACE;
    }

    struct stat st;
    if (lstat(dest_resolved, &st) == 0) {
        return FILE_OP_ALREADY_EXISTS;
    }

    return copy_recursive(src_resolved, dest_resolved);
}

FileOperationResult file_move(const WorkspaceRoot *src_root, const char *src_relative_path,
                               const WorkspaceRoot *dest_root, const char *dest_relative_path) {
    if (!leaf_name_is_valid(dest_relative_path)) {
        return FILE_OP_INVALID_NAME;
    }
    char src_resolved[4096];
    if (!workspace_root_resolve_path(src_root, src_relative_path, src_resolved, sizeof(src_resolved))) {
        return FILE_OP_OUTSIDE_WORKSPACE;
    }
    char dest_resolved[4096];
    if (!workspace_root_resolve_path(dest_root, dest_relative_path, dest_resolved, sizeof(dest_resolved))) {
        return FILE_OP_OUTSIDE_WORKSPACE;
    }

    /* rename() itself would silently overwrite an existing destination -
     * check first (accepted as non-atomic; not a realistic race for a
     * single-user local interactive tool - same acceptance the old
     * file_rename already documented). */
    struct stat st;
    if (lstat(dest_resolved, &st) == 0) {
        return FILE_OP_ALREADY_EXISTS;
    }
    if (lstat(src_resolved, &st) != 0) {
        return result_from_errno(errno);
    }

    if (rename(src_resolved, dest_resolved) == 0) {
        return FILE_OP_OK;
    }
    if (errno != EXDEV) {
        return result_from_errno(errno);
    }

    /* Genuinely different filesystems - copy then remove the original.
     * A failed copy leaves the original completely untouched (copy_recursive
     * doesn't partially delete anything on its own failure paths). */
    FileOperationResult copy_result = copy_recursive(src_resolved, dest_resolved);
    if (copy_result != FILE_OP_OK) {
        return copy_result;
    }
    return remove_recursive(src_resolved);
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
