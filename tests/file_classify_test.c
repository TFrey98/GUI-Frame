/*
 * Exercises file_classify() - a plain text file, a file with an
 * embedded NUL byte, an empty file, a directory path, a nonexistent
 * path, and an executable shebang script - proving classification
 * never uses the extension or the executable bit to decide
 * text-vs-binary, only the sniffed byte content.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files/file_classify.h"

static void write_file_bytes(const char *path, const char *content, size_t len) {
    FILE *f = fopen(path, "wb");
    if (f) {
        if (len > 0) {
            fwrite(content, 1, len, f);
        }
        fclose(f);
    }
}

int main(void) {
    char root_template[] = "/tmp/fc_test_root_XXXXXX";
    char *root_dir = mkdtemp(root_template);
    if (!root_dir) {
        fprintf(stderr, "file_classify_test: mkdtemp failed\n");
        return 1;
    }

    int status = 0;
    char path[4200];

    /* A plain text file classifies as EDITOR/likely_text. */
    snprintf(path, sizeof(path), "%s/plain.txt", root_dir);
    write_file_bytes(path, "hello world\nsecond line\n", 24);
    FileClassification c = file_classify(path, false);
    if (c.target != FILE_TARGET_EDITOR || !c.likely_text) {
        fprintf(stderr, "file_classify_test: plain text file should be FILE_TARGET_EDITOR/likely_text\n");
        status = 1;
    }
    if (c.executable) {
        fprintf(stderr, "file_classify_test: executable=false should be passed through unchanged\n");
        status = 1;
    }

    /* A file with an embedded NUL byte classifies as BINARY_INFO. */
    snprintf(path, sizeof(path), "%s/binary.dat", root_dir);
    char nul_content[8] = {'a', 'b', '\0', 'c', 'd', 'e', 'f', 'g'};
    write_file_bytes(path, nul_content, sizeof(nul_content));
    c = file_classify(path, false);
    if (c.target != FILE_TARGET_BINARY_INFO || c.likely_text) {
        fprintf(stderr, "file_classify_test: a NUL-byte file should be FILE_TARGET_BINARY_INFO/!likely_text\n");
        status = 1;
    }

    /* An empty file is trivially text. */
    snprintf(path, sizeof(path), "%s/empty.txt", root_dir);
    write_file_bytes(path, NULL, 0);
    c = file_classify(path, false);
    if (c.target != FILE_TARGET_EDITOR) {
        fprintf(stderr, "file_classify_test: an empty file should be FILE_TARGET_EDITOR\n");
        status = 1;
    }

    /* A directory path is unsupported. */
    char dir_path[4200];
    snprintf(dir_path, sizeof(dir_path), "%s/a_dir", root_dir);
    mkdir(dir_path, 0755);
    c = file_classify(dir_path, false);
    if (c.target != FILE_TARGET_UNSUPPORTED) {
        fprintf(stderr, "file_classify_test: a directory path should be FILE_TARGET_UNSUPPORTED\n");
        status = 1;
    }

    /* A nonexistent path is unsupported. */
    snprintf(path, sizeof(path), "%s/does_not_exist.txt", root_dir);
    c = file_classify(path, false);
    if (c.target != FILE_TARGET_UNSUPPORTED) {
        fprintf(stderr, "file_classify_test: a nonexistent path should be FILE_TARGET_UNSUPPORTED\n");
        status = 1;
    }

    /* An executable shebang script is still text - the executable bit
     * must never influence the text/binary decision. */
    snprintf(path, sizeof(path), "%s/script.sh", root_dir);
    const char *script = "#!/bin/sh\necho hello\n";
    write_file_bytes(path, script, strlen(script));
    chmod(path, 0755);
    c = file_classify(path, true);
    if (c.target != FILE_TARGET_EDITOR || !c.likely_text) {
        fprintf(stderr, "file_classify_test: an executable shebang script should still be FILE_TARGET_EDITOR\n");
        status = 1;
    }
    if (!c.executable) {
        fprintf(stderr, "file_classify_test: executable=true should be passed through unchanged\n");
        status = 1;
    }

    /* Cleanup. */
    snprintf(path, sizeof(path), "%s/plain.txt", root_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/binary.dat", root_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/empty.txt", root_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/script.sh", root_dir);
    unlink(path);
    rmdir(dir_path);
    rmdir(root_dir);

    if (status == 0) {
        printf("file_classify_test: text/binary/empty/directory/nonexistent/executable-script classification "
               "all verified\n");
    }
    return status;
}
