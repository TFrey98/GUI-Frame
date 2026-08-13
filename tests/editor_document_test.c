/*
 * Exercises editor_document_open()/editor_document_destroy() against a
 * real mkdtemp()'d WorkspaceRoot: a real small text file with contents
 * loaded, load_contents=false (metadata only, no contents), a file
 * larger than EDITOR_DOCUMENT_MAX_SIZE (content_size caps exactly
 * there and read_only is forced true even with a false hint), and a
 * nonexistent/outside-root path returning NULL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files/editor_document.h"
#include "files/workspace_root.h"

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
    char root_template[] = "/tmp/ed_test_root_XXXXXX";
    char *root_dir = mkdtemp(root_template);
    if (!root_dir) {
        fprintf(stderr, "editor_document_test: mkdtemp failed\n");
        return 1;
    }

    WorkspaceRoot root;
    if (!realpath(root_dir, root.canonical_path)) {
        fprintf(stderr, "editor_document_test: realpath failed\n");
        return 1;
    }
    strcpy(root.display_path, root.canonical_path);

    int status = 0;
    char path[4200];

    /* A real small text file, contents loaded. */
    snprintf(path, sizeof(path), "%s/small.txt", root.canonical_path);
    const char *small_content = "hello world\n";
    write_file_bytes(path, small_content, strlen(small_content));

    EditorDocument *doc = editor_document_open(&root, "small.txt", false, true);
    if (!doc) {
        fprintf(stderr, "editor_document_test: opening small.txt should succeed\n");
        status = 1;
    } else {
        if (!doc->contents || strcmp(doc->contents, small_content) != 0) {
            fprintf(stderr, "editor_document_test: contents should match the file's content\n");
            status = 1;
        }
        if (doc->content_size != strlen(small_content)) {
            fprintf(stderr, "editor_document_test: content_size should match the file's size\n");
            status = 1;
        }
        if (doc->read_only) {
            fprintf(stderr, "editor_document_test: read_only should reflect the false hint\n");
            status = 1;
        }
        if (doc->modified) {
            fprintf(stderr, "editor_document_test: modified should start false\n");
            status = 1;
        }
        if (strcmp(doc->display_name, "small.txt") != 0) {
            fprintf(stderr, "editor_document_test: display_name should be the basename\n");
            status = 1;
        }
        editor_document_destroy(doc);
    }

    /* read_only_hint=true is honored too. */
    doc = editor_document_open(&root, "small.txt", true, true);
    if (!doc || !doc->read_only) {
        fprintf(stderr, "editor_document_test: read_only should reflect a true hint\n");
        status = 1;
    }
    editor_document_destroy(doc);

    /* load_contents=false - metadata only, contents stays NULL,
     * content_size still matches the real file size, read_only forced
     * true regardless of hint. */
    doc = editor_document_open(&root, "small.txt", false, false);
    if (!doc) {
        fprintf(stderr, "editor_document_test: opening with load_contents=false should succeed\n");
        status = 1;
    } else {
        if (doc->contents != NULL) {
            fprintf(stderr, "editor_document_test: contents should stay NULL when load_contents=false\n");
            status = 1;
        }
        struct stat st;
        stat(path, &st);
        if (doc->content_size != (size_t)st.st_size) {
            fprintf(stderr, "editor_document_test: content_size should match the real file size via stat()\n");
            status = 1;
        }
        if (!doc->read_only) {
            fprintf(stderr, "editor_document_test: read_only should be forced true when load_contents=false\n");
            status = 1;
        }
        editor_document_destroy(doc);
    }

    /* A file larger than EDITOR_DOCUMENT_MAX_SIZE - content_size caps
     * exactly there, read_only forced true even with a false hint. */
    snprintf(path, sizeof(path), "%s/big.bin", root.canonical_path);
    size_t big_size = EDITOR_DOCUMENT_MAX_SIZE + 4096;
    char *big_content = malloc(big_size);
    memset(big_content, 'x', big_size);
    write_file_bytes(path, big_content, big_size);
    free(big_content);

    doc = editor_document_open(&root, "big.bin", false, true);
    if (!doc) {
        fprintf(stderr, "editor_document_test: opening big.bin should succeed\n");
        status = 1;
    } else {
        if (doc->content_size != EDITOR_DOCUMENT_MAX_SIZE) {
            fprintf(stderr, "editor_document_test: content_size should cap exactly at EDITOR_DOCUMENT_MAX_SIZE, "
                            "got %zu\n", doc->content_size);
            status = 1;
        }
        if (!doc->read_only) {
            fprintf(stderr, "editor_document_test: read_only should be forced true for a truncated load, even "
                            "with a false hint\n");
            status = 1;
        }
        editor_document_destroy(doc);
    }

    /* A nonexistent path returns NULL. */
    doc = editor_document_open(&root, "does_not_exist.txt", false, true);
    if (doc) {
        fprintf(stderr, "editor_document_test: opening a nonexistent path should return NULL\n");
        editor_document_destroy(doc);
        status = 1;
    }

    /* A path escaping the workspace root returns NULL. */
    doc = editor_document_open(&root, "../escape.txt", false, true);
    if (doc) {
        fprintf(stderr, "editor_document_test: opening a path outside the root should return NULL\n");
        editor_document_destroy(doc);
        status = 1;
    }

    /* Cleanup. */
    snprintf(path, sizeof(path), "%s/small.txt", root.canonical_path);
    unlink(path);
    snprintf(path, sizeof(path), "%s/big.bin", root.canonical_path);
    unlink(path);
    rmdir(root.canonical_path);

    if (status == 0) {
        printf("editor_document_test: text load, metadata-only load, large-file truncation/read-only-forcing, "
               "and nonexistent/outside-root rejection all verified\n");
    }
    return status;
}
