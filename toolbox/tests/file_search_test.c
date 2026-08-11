/*
 * Exercises file_search_scan() - matching a file or folder by its own
 * name, case-sensitive/case-insensitive matching, a matched folder
 * still being recursed into for further nested matches, a symlink's own
 * name being matchable but its target never followed/recursed into,
 * max_matches respected, and an empty query returning zero matches -
 * against a real mkdtemp()'d WorkspaceRoot.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files/file_search.h"

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static bool has_match(FileSearchMatch *matches, int count, const char *relative_path, bool is_dir) {
    for (int i = 0; i < count; i++) {
        if (strcmp(matches[i].relative_path, relative_path) == 0 && matches[i].is_dir == is_dir) {
            return true;
        }
    }
    return false;
}

int main(void) {
    char root_template[] = "/tmp/fs_test_root_XXXXXX";
    char *root_dir = mkdtemp(root_template);
    if (!root_dir) {
        fprintf(stderr, "file_search_test: mkdtemp failed\n");
        return 1;
    }

    WorkspaceRoot root;
    if (!realpath(root_dir, root.canonical_path)) {
        fprintf(stderr, "file_search_test: realpath failed\n");
        return 1;
    }
    strcpy(root.display_path, root.canonical_path);

    int status = 0;

    /* A file matched by its own (partial) name, case-sensitive. */
    char needle_file[4200];
    snprintf(needle_file, sizeof(needle_file), "%s/needlefile.txt", root.canonical_path);
    write_file(needle_file, "irrelevant content\n");

    /* A folder matched by its own name - and still recursed into for a
     * further nested match, proving a directory match doesn't stop the
     * walk from descending into it. */
    char needle_dir[4200];
    snprintf(needle_dir, sizeof(needle_dir), "%s/needledir", root.canonical_path);
    mkdir(needle_dir, 0755);
    char nested_file[4300];
    snprintf(nested_file, sizeof(nested_file), "%s/needledir/inner_needle.txt", root.canonical_path);
    write_file(nested_file, "x\n");

    /* A non-matching file, to prove the scan doesn't just return
     * everything. */
    char other_file[4200];
    snprintf(other_file, sizeof(other_file), "%s/plain.txt", root.canonical_path);
    write_file(other_file, "x\n");

    FileSearchMatch matches[64];
    int n = file_search_scan(&root, "needle", true, matches, 64);
    if (n != 3) {
        fprintf(stderr, "file_search_test: case-sensitive 'needle' should find exactly 3 matches, got %d\n", n);
        status = 1;
    }
    if (!has_match(matches, n, "needlefile.txt", false)) {
        fprintf(stderr, "file_search_test: expected a file match 'needlefile.txt'\n");
        status = 1;
    }
    if (!has_match(matches, n, "needledir", true)) {
        fprintf(stderr, "file_search_test: expected a directory match 'needledir'\n");
        status = 1;
    }
    if (!has_match(matches, n, "needledir/inner_needle.txt", false)) {
        fprintf(stderr, "file_search_test: a matched directory should still be recursed into for nested matches\n");
        status = 1;
    }
    if (has_match(matches, n, "plain.txt", false)) {
        fprintf(stderr, "file_search_test: 'plain.txt' should never match 'needle'\n");
        status = 1;
    }

    /* Case-insensitive matching. */
    char upper_file[4200];
    snprintf(upper_file, sizeof(upper_file), "%s/NEEDLEUPPER.txt", root.canonical_path);
    write_file(upper_file, "x\n");

    n = file_search_scan(&root, "needle", false, matches, 64);
    if (!has_match(matches, n, "NEEDLEUPPER.txt", false)) {
        fprintf(stderr, "file_search_test: case-insensitive search should match 'NEEDLEUPPER.txt'\n");
        status = 1;
    }
    n = file_search_scan(&root, "needle", true, matches, 64);
    if (has_match(matches, n, "NEEDLEUPPER.txt", false)) {
        fprintf(stderr, "file_search_test: case-sensitive search should not match 'NEEDLEUPPER.txt'\n");
        status = 1;
    }

    /* A symlink's own name is matchable, but its target is never
     * followed/recursed into - a real directory outside the workspace,
     * with its own "needle"-matching file inside, must never surface. */
    char outside_template[] = "/tmp/fs_test_outside_XXXXXX";
    char *outside_dir = mkdtemp(outside_template);
    char outside_nested[4200];
    snprintf(outside_nested, sizeof(outside_nested), "%s/needle_outside.txt", outside_dir);
    write_file(outside_nested, "x\n");
    char link_path[4200];
    snprintf(link_path, sizeof(link_path), "%s/needlelink", root.canonical_path);
    symlink(outside_dir, link_path);

    n = file_search_scan(&root, "needle", false, matches, 64);
    if (!has_match(matches, n, "needlelink", false)) {
        fprintf(stderr, "file_search_test: a symlink's own name should be matchable\n");
        status = 1;
    }
    for (int i = 0; i < n; i++) {
        if (strstr(matches[i].relative_path, "needlelink/") != NULL) {
            fprintf(stderr, "file_search_test: a symlink must never be followed/recursed into\n");
            status = 1;
        }
    }

    /* max_matches is respected. */
    FileSearchMatch capped[2];
    n = file_search_scan(&root, "needle", false, capped, 2);
    if (n != 2) {
        fprintf(stderr, "file_search_test: max_matches=2 should cap the result count at 2, got %d\n", n);
        status = 1;
    }

    /* An empty query returns zero matches rather than matching
     * everything. */
    n = file_search_scan(&root, "", false, matches, 64);
    if (n != 0) {
        fprintf(stderr, "file_search_test: an empty query should return zero matches, got %d\n", n);
        status = 1;
    }

    /* Cleanup. */
    unlink(needle_file);
    unlink(nested_file);
    rmdir(needle_dir);
    unlink(other_file);
    unlink(upper_file);
    unlink(link_path);
    unlink(outside_nested);
    rmdir(outside_dir);
    rmdir(root.canonical_path);

    if (status == 0) {
        printf("file_search_test: file/folder name matching, case-sensitivity, recursion-into-matched-folders, "
               "symlink safety, max_matches, and empty-query behavior all verified\n");
    }
    return status;
}
