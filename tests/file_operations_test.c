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

    /* file_copy: single file, rejects an existing destination, and a
     * recursive directory copy that never follows a symlink into
     * copying its target's content - the symlink entry itself is
     * recreated (readlink()+symlink()), still pointing at the same real
     * target, mirroring file_delete's own symlink discipline. */
    char copy_src_path[4200];
    snprintf(copy_src_path, sizeof(copy_src_path), "%s/copy_src.txt", root.canonical_path);
    write_file(copy_src_path, "copy me");
    if (file_copy(&root, "copy_src.txt", &root, "copy_dest.txt") != FILE_OP_OK) {
        fprintf(stderr, "file_operations_test: file_copy(copy_src.txt -> copy_dest.txt) should succeed\n");
        status = 1;
    }
    char copy_dest_path[4200];
    snprintf(copy_dest_path, sizeof(copy_dest_path), "%s/copy_dest.txt", root.canonical_path);
    char copy_contents[64] = {0};
    if (read_file_contents(copy_dest_path, copy_contents, sizeof(copy_contents)) != 0 ||
        strcmp(copy_contents, "copy me") != 0) {
        fprintf(stderr, "file_operations_test: copied file should have the source's content, got '%s'\n",
                copy_contents);
        status = 1;
    }
    if (access(copy_src_path, F_OK) != 0) {
        fprintf(stderr, "file_operations_test: file_copy must leave the source untouched\n");
        status = 1;
    }
    if (file_copy(&root, "copy_src.txt", &root, "copy_dest.txt") != FILE_OP_ALREADY_EXISTS) {
        fprintf(stderr,
                "file_operations_test: file_copy onto an existing destination should be FILE_OP_ALREADY_EXISTS\n");
        status = 1;
    }

    char copydir_path[4200], copydir_copy_path[4200];
    snprintf(copydir_path, sizeof(copydir_path), "%s/copydir", root.canonical_path);
    snprintf(copydir_copy_path, sizeof(copydir_copy_path), "%s/copydir_copy", root.canonical_path);
    directory_create(&root, "copydir");
    char copydir_nested[4300], copydir_link[4300];
    snprintf(copydir_nested, sizeof(copydir_nested), "%s/copydir/nested.txt", root.canonical_path);
    write_file(copydir_nested, "nested content");
    snprintf(copydir_link, sizeof(copydir_link), "%s/copydir/link", root.canonical_path);
    symlink(copy_src_path, copydir_link);

    if (file_copy(&root, "copydir", &root, "copydir_copy") != FILE_OP_OK) {
        fprintf(stderr, "file_operations_test: recursive file_copy(copydir -> copydir_copy) should succeed\n");
        status = 1;
    }
    char copydir_copy_nested[4300];
    snprintf(copydir_copy_nested, sizeof(copydir_copy_nested), "%s/copydir_copy/nested.txt", root.canonical_path);
    char nested_copy_contents[64] = {0};
    if (read_file_contents(copydir_copy_nested, nested_copy_contents, sizeof(nested_copy_contents)) != 0 ||
        strcmp(nested_copy_contents, "nested content") != 0) {
        fprintf(stderr, "file_operations_test: recursively copied file should keep its content, got '%s'\n",
                nested_copy_contents);
        status = 1;
    }
    char copydir_copy_link[4300];
    snprintf(copydir_copy_link, sizeof(copydir_copy_link), "%s/copydir_copy/link", root.canonical_path);
    struct stat link_st;
    char link_readback[4200] = {0};
    ssize_t link_len = readlink(copydir_copy_link, link_readback, sizeof(link_readback) - 1);
    if (link_len >= 0) {
        link_readback[link_len] = '\0';
    }
    if (lstat(copydir_copy_link, &link_st) != 0 || !S_ISLNK(link_st.st_mode) || link_len < 0 ||
        strcmp(link_readback, copy_src_path) != 0) {
        fprintf(stderr, "file_operations_test: a symlink inside a recursively copied directory should be recreated "
                        "as a symlink pointing at the same real target, not followed\n");
        status = 1;
    }

    /* file_move: same-root move behaves identically to file_rename
     * (which now delegates to it); a cross-root move between two
     * independent WorkspaceRoots proves the two-independent-roots path
     * works end to end, regardless of whether it happened to hit the
     * rename() fast path or the copy+delete fallback - a real EXDEV
     * can't be reliably forced without a second real mount point, so
     * that specific branch isn't independently exercised here, a known
     * accepted gap. */
    char move_src_path[4200];
    snprintf(move_src_path, sizeof(move_src_path), "%s/move_src.txt", root.canonical_path);
    write_file(move_src_path, "move me");
    if (file_move(&root, "move_src.txt", &root, "move_dest.txt") != FILE_OP_OK) {
        fprintf(stderr, "file_operations_test: file_move(move_src.txt -> move_dest.txt) should succeed\n");
        status = 1;
    }
    if (access(move_src_path, F_OK) == 0) {
        fprintf(stderr, "file_operations_test: file_move must remove the source\n");
        status = 1;
    }
    char move_dest_path[4200];
    snprintf(move_dest_path, sizeof(move_dest_path), "%s/move_dest.txt", root.canonical_path);
    char move_contents[64] = {0};
    if (read_file_contents(move_dest_path, move_contents, sizeof(move_contents)) != 0 ||
        strcmp(move_contents, "move me") != 0) {
        fprintf(stderr, "file_operations_test: moved file should keep its content, got '%s'\n", move_contents);
        status = 1;
    }

    write_file(move_src_path, "recreated for collision check");
    if (file_move(&root, "move_src.txt", &root, "move_dest.txt") != FILE_OP_ALREADY_EXISTS) {
        fprintf(stderr,
                "file_operations_test: file_move onto an existing destination should be FILE_OP_ALREADY_EXISTS\n");
        status = 1;
    }
    if (access(move_src_path, F_OK) != 0) {
        fprintf(stderr, "file_operations_test: a rejected file_move must leave the source untouched\n");
        status = 1;
    }

    char root2_template[] = "/tmp/fo_test_root2_XXXXXX";
    char *root2_dir = mkdtemp(root2_template);
    WorkspaceRoot root2;
    if (!root2_dir || !realpath(root2_dir, root2.canonical_path)) {
        fprintf(stderr, "file_operations_test: mkdtemp/realpath (root2) failed\n");
        return 1;
    }
    strcpy(root2.display_path, root2.canonical_path);

    if (file_move(&root, "move_src.txt", &root2, "cross_root_dest.txt") != FILE_OP_OK) {
        fprintf(stderr, "file_operations_test: cross-root file_move should succeed\n");
        status = 1;
    }
    if (access(move_src_path, F_OK) == 0) {
        fprintf(stderr, "file_operations_test: cross-root file_move must remove the source\n");
        status = 1;
    }
    char cross_root_dest_path[4200];
    snprintf(cross_root_dest_path, sizeof(cross_root_dest_path), "%s/cross_root_dest.txt", root2.canonical_path);
    char cross_root_contents[64] = {0};
    if (read_file_contents(cross_root_dest_path, cross_root_contents, sizeof(cross_root_contents)) != 0 ||
        strcmp(cross_root_contents, "recreated for collision check") != 0) {
        fprintf(stderr, "file_operations_test: cross-root moved file should keep its content, got '%s'\n",
                cross_root_contents);
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
    if (file_copy(&root, "copy_dest.txt", &root, "../escape_copy.txt") != FILE_OP_OUTSIDE_WORKSPACE) {
        fprintf(stderr, "file_operations_test: file_copy onto '..' should be FILE_OP_OUTSIDE_WORKSPACE\n");
        status = 1;
    }
    if (file_move(&root, "move_dest.txt", &root, "../escape_move.txt") != FILE_OP_OUTSIDE_WORKSPACE) {
        fprintf(stderr, "file_operations_test: file_move onto '..' should be FILE_OP_OUTSIDE_WORKSPACE\n");
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
    unlink(copy_src_path);
    unlink(copy_dest_path);
    unlink(copydir_nested);
    unlink(copydir_link);
    rmdir(copydir_path);
    unlink(copydir_copy_nested);
    unlink(copydir_copy_link);
    rmdir(copydir_copy_path);
    unlink(move_dest_path);
    rmdir(root.canonical_path);
    unlink(cross_root_dest_path);
    rmdir(root2.canonical_path);

    if (status == 0) {
        printf("file_operations_test: create/rename/delete/copy/move semantics, duplicate/invalid-name rejection, "
               "recursive-copy/delete symlink safety, cross-root move, and containment all verified\n");
    }
    return status;
}
