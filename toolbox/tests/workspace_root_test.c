/*
 * Exercises workspace_root_resolve_path() - the containment function
 * every filesystem operation in the file-explorer subsystem must go
 * through - against the exact traversal/escape cases the spec calls
 * out: ".." components, absolute paths, symlink escapes, and the
 * "prefix-matching mistake" where a naive strncmp containment check
 * would wrongly accept a sibling directory like "<root>-other".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files/workspace_root.h"

static void write_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (f) {
        fclose(f);
    }
}

static int expect_success(int *status, WorkspaceRoot *root, const char *relative_path, const char *expected_suffix) {
    char resolved[4096];
    if (!workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
        fprintf(stderr, "workspace_root_test: expected '%s' to resolve, but it was rejected\n", relative_path);
        *status = 1;
        return 0;
    }
    size_t root_len = strlen(root->canonical_path);
    if (strncmp(resolved, root->canonical_path, root_len) != 0) {
        fprintf(stderr, "workspace_root_test: '%s' resolved outside root: %s\n", relative_path, resolved);
        *status = 1;
        return 0;
    }
    if (expected_suffix && strcmp(resolved + root_len, expected_suffix) != 0) {
        fprintf(stderr, "workspace_root_test: '%s' resolved to '%s', expected suffix '%s'\n", relative_path,
                resolved, expected_suffix);
        *status = 1;
        return 0;
    }
    return 1;
}

static int expect_rejected(int *status, WorkspaceRoot *root, const char *relative_path) {
    char resolved[4096];
    if (workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
        fprintf(stderr, "workspace_root_test: expected '%s' to be rejected, but it resolved to '%s'\n",
                relative_path, resolved);
        *status = 1;
        return 0;
    }
    return 1;
}

int main(void) {
    char root_template[] = "/tmp/wr_test_root_XXXXXX";
    char *root_dir = mkdtemp(root_template);
    if (!root_dir) {
        fprintf(stderr, "workspace_root_test: mkdtemp (root) failed\n");
        return 1;
    }

    char outside_template[] = "/tmp/wr_test_outside_XXXXXX";
    char *outside_dir = mkdtemp(outside_template);
    if (!outside_dir) {
        fprintf(stderr, "workspace_root_test: mkdtemp (outside) failed\n");
        return 1;
    }

    WorkspaceRoot root;
    if (!realpath(root_dir, root.canonical_path)) {
        fprintf(stderr, "workspace_root_test: realpath(root) failed\n");
        return 1;
    }
    strcpy(root.display_path, root.canonical_path);

    char notes_dir[4200], notes_file[4300];
    snprintf(notes_dir, sizeof(notes_dir), "%s/notes", root.canonical_path);
    snprintf(notes_file, sizeof(notes_file), "%s/file.txt", notes_dir);
    mkdir(notes_dir, 0755);
    write_file(notes_file);

    char outside_secret[4200];
    snprintf(outside_secret, sizeof(outside_secret), "%s/secret.txt", outside_dir);
    write_file(outside_secret);

    char escape_link[4200];
    snprintf(escape_link, sizeof(escape_link), "%s/escape", root.canonical_path);
    symlink(outside_dir, escape_link);

    /* A sibling directory sharing root's path as a raw string prefix -
     * the case a naive strncmp(resolved, root, strlen(root)) containment
     * check would wrongly accept. */
    char sibling_dir[4200];
    snprintf(sibling_dir, sizeof(sibling_dir), "%s-other", root.canonical_path);
    mkdir(sibling_dir, 0755);
    char sibling_secret[4300];
    snprintf(sibling_secret, sizeof(sibling_secret), "%s/secret.txt", sibling_dir);
    write_file(sibling_secret);

    char escape_prefix_link[4200];
    snprintf(escape_prefix_link, sizeof(escape_prefix_link), "%s/escape_prefix", root.canonical_path);
    symlink(sibling_dir, escape_prefix_link);

    int status = 0;

    expect_success(&status, &root, "notes/file.txt", "/notes/file.txt");
    expect_success(&status, &root, "./notes/file.txt", "/notes/file.txt");
    expect_rejected(&status, &root, "notes/../file.txt");
    expect_rejected(&status, &root, "../../outside.txt");
    expect_rejected(&status, &root, "/etc/passwd");
    expect_rejected(&status, &root, "escape/secret.txt");
    expect_rejected(&status, &root, "escape_prefix/secret.txt");
    expect_success(&status, &root, "notes/newfile.txt", "/notes/newfile.txt");
    expect_success(&status, &root, "newfile.txt", "/newfile.txt");
    expect_rejected(&status, &root, "missing_dir/newfile.txt");
    expect_rejected(&status, &root, "");
    expect_rejected(&status, &root, "..");

    unlink(escape_prefix_link);
    unlink(escape_link);
    unlink(sibling_secret);
    rmdir(sibling_dir);
    unlink(outside_secret);
    rmdir(outside_dir);
    unlink(notes_file);
    rmdir(notes_dir);
    rmdir(root.canonical_path);

    if (status == 0) {
        printf("workspace_root_test: path containment verified (traversal, absolute, symlink escape, "
               "prefix-matching mistake all rejected; valid and not-yet-existing paths resolve correctly)\n");
    }
    return status;
}
