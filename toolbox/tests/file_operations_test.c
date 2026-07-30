/*
 * Exercises FileOperations - file_create/directory_create/file_rename/
 * file_delete/file_operations_directory_is_empty - against a real
 * mkdtemp()'d WorkspaceRoot: duplicate names, invalid leaf names,
 * non-empty-directory delete semantics (non-recursive rejects,
 * recursive succeeds without following a symlink into deleting its
 * target), and that every function inherits workspace_root_resolve_path()'s
 * containment (a "../" escape is rejected, not just an in-bounds path).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files/file_operations.h"
#include "files/workspace_root.h"

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) {
        if (content) {
            fputs(content, f);
        }
        fclose(f);
    }
}

static int read_file_contents(const char *path, char *out, size_t out_size) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    size_t n = fread(out, 1, out_size - 1, f);
    out[n] = '\0';
    fclose(f);
    return 0;
}

int main(void) {
    char root_template[] = "/tmp/fo_test_root_XXXXXX";
    char *root_dir = mkdtemp(root_template);
    if (!root_dir) {
        fprintf(stderr, "file_operations_test: mkdtemp failed\n");
        return 1;
    }

    WorkspaceRoot root;
    if (!realpath(root_dir, root.canonical_path)) {
        fprintf(stderr, "file_operations_test: realpath failed\n");
        return 1;
    }
    strcpy(root.display_path, root.canonical_path);

    int status = 0;

    /* file_create: succeeds, then a duplicate is rejected cleanly. */
    if (file_create(&root, "a.txt") != FILE_OP_OK) {
        fprintf(stderr, "file_operations_test: file_create(a.txt) should succeed\n");
        status = 1;
    }
    if (file_create(&root, "a.txt") != FILE_OP_ALREADY_EXISTS) {
        fprintf(stderr, "file_operations_test: duplicate file_create should be FILE_OP_ALREADY_EXISTS\n");
        status = 1;
    }

    /* directory_create: succeeds, then a duplicate is rejected. */
    if (directory_create(&root, "sub") != FILE_OP_OK) {
        fprintf(stderr, "file_operations_test: directory_create(sub) should succeed\n");
        status = 1;
    }
    if (directory_create(&root, "sub") != FILE_OP_ALREADY_EXISTS) {
        fprintf(stderr, "file_operations_test: duplicate directory_create should be FILE_OP_ALREADY_EXISTS\n");
        status = 1;
    }

    /* Invalid leaf names - the doc's "invalid filename characters"
     * criterion, scoped to what's actually invalid for a single path
     * component on Linux: empty, "." and "..". (A literal '/' can't
     * appear *within* a leaf component by definition - strrchr('/')
     * is exactly how the leaf is extracted - so a caller that wants to
     * reject a raw typed "/" before it's ever joined into a path, e.g.
     * an inline-edit text box, validates that separately, upstream of
     * this API.) */
    if (file_create(&root, "sub/") != FILE_OP_INVALID_NAME) {
        fprintf(stderr, "file_operations_test: trailing-slash (empty leaf) should be FILE_OP_INVALID_NAME\n");
        status = 1;
    }
    if (file_create(&root, ".") != FILE_OP_INVALID_NAME) {
        fprintf(stderr, "file_operations_test: '.' should be FILE_OP_INVALID_NAME\n");
        status = 1;
    }
    if (file_create(&root, "..") != FILE_OP_INVALID_NAME) {
        fprintf(stderr, "file_operations_test: '..' should be FILE_OP_INVALID_NAME\n");
        status = 1;
    }

    /* file_rename: moves content, rejects renaming onto an existing name
     * (leaving the original untouched), and updates what resolves where. */
    char a_path[4200], b_path[4200];
    snprintf(a_path, sizeof(a_path), "%s/a.txt", root.canonical_path);
    write_file(a_path, "hello");

    if (file_rename(&root, "a.txt", "b.txt") != FILE_OP_OK) {
        fprintf(stderr, "file_operations_test: file_rename(a.txt -> b.txt) should succeed\n");
        status = 1;
    }
    snprintf(b_path, sizeof(b_path), "%s/b.txt", root.canonical_path);
    if (access(a_path, F_OK) == 0) {
        fprintf(stderr, "file_operations_test: old name should no longer exist after rename\n");
        status = 1;
    }
    char contents[64] = {0};
    if (read_file_contents(b_path, contents, sizeof(contents)) != 0 || strcmp(contents, "hello") != 0) {
        fprintf(stderr, "file_operations_test: renamed file should keep its content, got '%s'\n", contents);
        status = 1;
    }

    write_file(a_path, "recreated");
    if (file_rename(&root, "a.txt", "b.txt") != FILE_OP_ALREADY_EXISTS) {
        fprintf(stderr, "file_operations_test: renaming onto an existing name should be FILE_OP_ALREADY_EXISTS\n");
        status = 1;
    }
    if (access(a_path, F_OK) != 0) {
        fprintf(stderr, "file_operations_test: a rejected rename must leave the source untouched\n");
        status = 1;
    }

    /* file_delete: a plain file; a non-empty directory rejected non-
     * recursively but succeeding recursively, without following a
     * symlink inside it into deleting its target. */
    if (file_delete(&root, "a.txt", false) != FILE_OP_OK) {
        fprintf(stderr, "file_operations_test: file_delete(a.txt) should succeed\n");
        status = 1;
    }

    char nested_file[4300], nested_link[4300];
    snprintf(nested_file, sizeof(nested_file), "%s/sub/nested.txt", root.canonical_path);
    write_file(nested_file, "x");

    char outside_template[] = "/tmp/fo_test_outside_XXXXXX";
    char *outside_dir = mkdtemp(outside_template);
    char outside_target[4200];
    snprintf(outside_target, sizeof(outside_target), "%s/target.txt", outside_dir);
    write_file(outside_target, "should survive");
    snprintf(nested_link, sizeof(nested_link), "%s/sub/link_out", root.canonical_path);
    symlink(outside_target, nested_link);

    if (file_operations_directory_is_empty(&root, "sub")) {
        fprintf(stderr, "file_operations_test: 'sub' should not be reported empty\n");
        status = 1;
    }
    if (file_delete(&root, "sub", false) != FILE_OP_DIRECTORY_NOT_EMPTY) {
        fprintf(stderr, "file_operations_test: non-recursive delete of a non-empty dir should be "
                        "FILE_OP_DIRECTORY_NOT_EMPTY\n");
        status = 1;
    }
    if (file_delete(&root, "sub", true) != FILE_OP_OK) {
        fprintf(stderr, "file_operations_test: recursive delete of 'sub' should succeed\n");
        status = 1;
    }
    char sub_path[4200];
    snprintf(sub_path, sizeof(sub_path), "%s/sub", root.canonical_path);
    if (access(sub_path, F_OK) == 0) {
        fprintf(stderr, "file_operations_test: 'sub' should be gone after recursive delete\n");
        status = 1;
    }
    if (access(outside_target, F_OK) != 0) {
        fprintf(stderr, "file_operations_test: recursive delete must not follow a symlink into deleting its "
                        "target\n");
        status = 1;
    }

    /* file_operations_directory_is_empty on an empty and a nonexistent
     * directory. */
    directory_create(&root, "empty_dir");
    if (!file_operations_directory_is_empty(&root, "empty_dir")) {
        fprintf(stderr, "file_operations_test: 'empty_dir' should be reported empty\n");
        status = 1;
    }
    if (file_operations_directory_is_empty(&root, "does_not_exist")) {
        fprintf(stderr, "file_operations_test: a nonexistent directory should not be reported empty\n");
        status = 1;
    }

    /* Containment is inherited from workspace_root_resolve_path() -
     * every function rejects a traversal attempt. */
    if (file_create(&root, "../escape.txt") != FILE_OP_OUTSIDE_WORKSPACE) {
        fprintf(stderr, "file_operations_test: file_create with '..' should be FILE_OP_OUTSIDE_WORKSPACE\n");
        status = 1;
    }
    if (directory_create(&root, "../escape_dir") != FILE_OP_OUTSIDE_WORKSPACE) {
        fprintf(stderr, "file_operations_test: directory_create with '..' should be FILE_OP_OUTSIDE_WORKSPACE\n");
        status = 1;
    }
    if (file_rename(&root, "b.txt", "../escape.txt") != FILE_OP_OUTSIDE_WORKSPACE) {
        fprintf(stderr, "file_operations_test: file_rename onto '..' should be FILE_OP_OUTSIDE_WORKSPACE\n");
        status = 1;
    }
    if (file_delete(&root, "../escape.txt", false) != FILE_OP_OUTSIDE_WORKSPACE) {
        fprintf(stderr, "file_operations_test: file_delete with '..' should be FILE_OP_OUTSIDE_WORKSPACE\n");
        status = 1;
    }

    /* Cleanup. */
    unlink(b_path);
    rmdir(sub_path);
    char empty_dir_path[4200];
    snprintf(empty_dir_path, sizeof(empty_dir_path), "%s/empty_dir", root.canonical_path);
    rmdir(empty_dir_path);
    unlink(outside_target);
    rmdir(outside_dir);
    rmdir(root.canonical_path);

    if (status == 0) {
        printf("file_operations_test: create/rename/delete semantics, duplicate/invalid-name rejection, "
               "recursive-delete symlink safety, and containment all verified\n");
    }
    return status;
}
