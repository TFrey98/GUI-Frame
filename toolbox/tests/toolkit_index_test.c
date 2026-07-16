/*
 * Proves toolkit_scan_directory only indexes the immediate contents of a
 * directory - a nested subfolder is recorded as a single entry, but its
 * own contents are never read or surfaced.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tools/toolkit_index.h"

static void write_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (f) {
        fclose(f);
    }
}

int main(void) {
    char root[] = "/tmp/toolkit_index_test_XXXXXX";
    if (!mkdtemp(root)) {
        fprintf(stderr, "toolkit_index_test: mkdtemp failed\n");
        return 1;
    }

    char file_a[512], file_b[512], subdir[512], nested[600];
    snprintf(file_a, sizeof(file_a), "%s/a.txt", root);
    snprintf(file_b, sizeof(file_b), "%s/b.txt", root);
    snprintf(subdir, sizeof(subdir), "%s/sub", root);
    snprintf(nested, sizeof(nested), "%s/nested.txt", subdir);

    write_file(file_a);
    write_file(file_b);
    mkdir(subdir, 0755);
    write_file(nested);

    ToolkitEntry entries[16];
    int count = toolkit_scan_directory(root, entries, 16);

    int status = 0;

    if (count != 3) {
        fprintf(stderr, "toolkit_index_test: expected 3 entries, got %d (nested file must not be surfaced)\n", count);
        status = 1;
    } else {
        if (!entries[0].is_dir || strcmp(entries[0].name, "sub") != 0) {
            fprintf(stderr, "toolkit_index_test: expected 'sub' directory sorted first\n");
            status = 1;
        }
        if (entries[1].is_dir || strcmp(entries[1].name, "a.txt") != 0) {
            fprintf(stderr, "toolkit_index_test: expected 'a.txt' as second entry\n");
            status = 1;
        }
        if (entries[2].is_dir || strcmp(entries[2].name, "b.txt") != 0) {
            fprintf(stderr, "toolkit_index_test: expected 'b.txt' as third entry\n");
            status = 1;
        }
    }

    for (int i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].path);
    }

    unlink(nested);
    rmdir(subdir);
    unlink(file_a);
    unlink(file_b);
    rmdir(root);

    if (status == 0) {
        printf("toolkit_index_test: non-recursive single-level scan verified\n");
    }
    return status;
}
