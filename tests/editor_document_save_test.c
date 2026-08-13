/*
 * Exercises editor_document_save() against a real mkdtemp()'d
 * WorkspaceRoot: in-place save round-trips content, clears modified,
 * and preserves permissions; a read_only doc is rejected outright; Save
 * As creates a new file while the old one survives untouched, and is
 * itself rejected onto an existing destination or an invalid/outside-
 * workspace path; and a forced I/O failure leaves the original file's
 * content byte-for-byte unchanged (the doc's "a failed save does not
 * destroy the original file").
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
    char root_template[] = "/tmp/eds_test_root_XXXXXX";
    char *root_dir = mkdtemp(root_template);
    if (!root_dir) {
        fprintf(stderr, "editor_document_save_test: mkdtemp failed\n");
        return 1;
    }

    WorkspaceRoot root;
    if (!realpath(root_dir, root.canonical_path)) {
        fprintf(stderr, "editor_document_save_test: realpath failed\n");
        return 1;
    }
    strcpy(root.display_path, root.canonical_path);

    int status = 0;
    char path[4300];

    /* In-place save round-trips content, clears modified, and preserves
     * permissions. */
    snprintf(path, sizeof(path), "%s/notes.txt", root.canonical_path);
    write_file_bytes(path, "original\n", strlen("original\n"));
    chmod(path, 0640);

    EditorDocument *doc = editor_document_open(&root, "notes.txt", false, true);
    if (!doc) {
        fprintf(stderr, "editor_document_save_test: opening notes.txt should succeed\n");
        return 1;
    }
    doc->modified = true;
    const char *new_content = "edited content\n";
    EditorSaveResult result = editor_document_save(&root, doc, NULL, new_content, strlen(new_content));
    if (result != EDITOR_SAVE_OK) {
        fprintf(stderr, "editor_document_save_test: in-place save should succeed, got %d\n", result);
        status = 1;
    }
    if (doc->modified) {
        fprintf(stderr, "editor_document_save_test: modified should be cleared after a successful save\n");
        status = 1;
    }
    if (!doc->contents || strcmp(doc->contents, new_content) != 0) {
        fprintf(stderr, "editor_document_save_test: doc->contents should reflect the saved text\n");
        status = 1;
    }
    char on_disk[64] = {0};
    if (read_file_contents(path, on_disk, sizeof(on_disk)) != 0 || strcmp(on_disk, new_content) != 0) {
        fprintf(stderr, "editor_document_save_test: file on disk should match the saved text, got '%s'\n", on_disk);
        status = 1;
    }
    struct stat st_after_save;
    stat(path, &st_after_save);
    if ((st_after_save.st_mode & 0777) != 0640) {
        fprintf(stderr, "editor_document_save_test: permissions should be preserved (0640), got %o\n",
                st_after_save.st_mode & 0777);
        status = 1;
    }
    editor_document_destroy(doc);

    /* A read_only doc is rejected outright and left untouched. */
    snprintf(path, sizeof(path), "%s/locked.txt", root.canonical_path);
    write_file_bytes(path, "cannot touch\n", strlen("cannot touch\n"));
    doc = editor_document_open(&root, "locked.txt", true, true);
    result = editor_document_save(&root, doc, NULL, "hacked\n", 7);
    if (result != EDITOR_SAVE_READ_ONLY) {
        fprintf(stderr, "editor_document_save_test: saving a read_only doc should be EDITOR_SAVE_READ_ONLY\n");
        status = 1;
    }
    char locked_on_disk[64] = {0};
    read_file_contents(path, locked_on_disk, sizeof(locked_on_disk));
    if (strcmp(locked_on_disk, "cannot touch\n") != 0) {
        fprintf(stderr, "editor_document_save_test: a rejected read-only save must leave the file untouched\n");
        status = 1;
    }
    editor_document_destroy(doc);

    /* Save As creates a new file while the old one survives untouched,
     * and updates relative_path/display_name. */
    snprintf(path, sizeof(path), "%s/original.txt", root.canonical_path);
    write_file_bytes(path, "keep me\n", strlen("keep me\n"));
    doc = editor_document_open(&root, "original.txt", false, true);
    const char *copy_content = "a fresh copy\n";
    result = editor_document_save(&root, doc, "copy.txt", copy_content, strlen(copy_content));
    if (result != EDITOR_SAVE_OK) {
        fprintf(stderr, "editor_document_save_test: Save As to a new path should succeed, got %d\n", result);
        status = 1;
    }
    if (strcmp(doc->relative_path, "copy.txt") != 0 || strcmp(doc->display_name, "copy.txt") != 0) {
        fprintf(stderr, "editor_document_save_test: Save As should update relative_path/display_name\n");
        status = 1;
    }
    char original_on_disk[64] = {0};
    read_file_contents(path, original_on_disk, sizeof(original_on_disk));
    if (strcmp(original_on_disk, "keep me\n") != 0) {
        fprintf(stderr, "editor_document_save_test: Save As must leave the old file untouched\n");
        status = 1;
    }
    char copy_path[4200], copy_on_disk[64] = {0};
    snprintf(copy_path, sizeof(copy_path), "%s/copy.txt", root.canonical_path);
    read_file_contents(copy_path, copy_on_disk, sizeof(copy_on_disk));
    if (strcmp(copy_on_disk, copy_content) != 0) {
        fprintf(stderr, "editor_document_save_test: the new Save As file should hold the saved text\n");
        status = 1;
    }
    editor_document_destroy(doc);

    /* Save As onto an existing destination is rejected; neither file
     * is touched. */
    snprintf(path, sizeof(path), "%s/source.txt", root.canonical_path);
    write_file_bytes(path, "source\n", strlen("source\n"));
    char dest_path[4200];
    snprintf(dest_path, sizeof(dest_path), "%s/dest.txt", root.canonical_path);
    write_file_bytes(dest_path, "dest\n", strlen("dest\n"));
    doc = editor_document_open(&root, "source.txt", false, true);
    result = editor_document_save(&root, doc, "dest.txt", "overwritten\n", 12);
    if (result != EDITOR_SAVE_ALREADY_EXISTS) {
        fprintf(stderr, "editor_document_save_test: Save As onto an existing file should be "
                        "EDITOR_SAVE_ALREADY_EXISTS\n");
        status = 1;
    }
    char dest_on_disk[64] = {0};
    read_file_contents(dest_path, dest_on_disk, sizeof(dest_on_disk));
    if (strcmp(dest_on_disk, "dest\n") != 0) {
        fprintf(stderr, "editor_document_save_test: a rejected Save As must leave the destination untouched\n");
        status = 1;
    }
    if (strcmp(doc->relative_path, "source.txt") != 0) {
        fprintf(stderr, "editor_document_save_test: a rejected Save As must leave doc's relative_path unchanged\n");
        status = 1;
    }
    editor_document_destroy(doc);

    /* Invalid leaf names and an outside-workspace path are both
     * rejected cleanly. */
    doc = editor_document_open(&root, "notes.txt", false, true);
    if (editor_document_save(&root, doc, "..", "x", 1) != EDITOR_SAVE_INVALID_NAME) {
        fprintf(stderr, "editor_document_save_test: Save As to '..' should be EDITOR_SAVE_INVALID_NAME\n");
        status = 1;
    }
    if (editor_document_save(&root, doc, "sub/", "x", 1) != EDITOR_SAVE_INVALID_NAME) {
        fprintf(stderr, "editor_document_save_test: Save As with a trailing slash should be "
                        "EDITOR_SAVE_INVALID_NAME\n");
        status = 1;
    }
    if (editor_document_save(&root, doc, "../escape.txt", "x", 1) != EDITOR_SAVE_OUTSIDE_WORKSPACE) {
        fprintf(stderr, "editor_document_save_test: Save As outside the workspace should be "
                        "EDITOR_SAVE_OUTSIDE_WORKSPACE\n");
        status = 1;
    }
    editor_document_destroy(doc);

    /* A forced I/O failure (a non-writable parent directory) leaves the
     * original file's content byte-for-byte unchanged. */
    char readonly_dir[4200];
    snprintf(readonly_dir, sizeof(readonly_dir), "%s/readonly_dir", root.canonical_path);
    mkdir(readonly_dir, 0755);
    snprintf(path, sizeof(path), "%s/target.txt", readonly_dir);
    write_file_bytes(path, "must survive\n", strlen("must survive\n"));
    chmod(readonly_dir, 0555); /* read+execute only - mkstemp() inside it must fail */

    char relative_target[64];
    snprintf(relative_target, sizeof(relative_target), "readonly_dir/target.txt");
    doc = editor_document_open(&root, relative_target, false, true);
    result = editor_document_save(&root, doc, NULL, "corrupted\n", 10);
    chmod(readonly_dir, 0755); /* restore before reading back / cleanup */
    if (result != EDITOR_SAVE_IO_ERROR) {
        fprintf(stderr, "editor_document_save_test: a save into a non-writable directory should be "
                        "EDITOR_SAVE_IO_ERROR, got %d\n", result);
        status = 1;
    }
    char survivor_on_disk[64] = {0};
    read_file_contents(path, survivor_on_disk, sizeof(survivor_on_disk));
    if (strcmp(survivor_on_disk, "must survive\n") != 0) {
        fprintf(stderr, "editor_document_save_test: a failed save must leave the original file untouched, "
                        "got '%s'\n", survivor_on_disk);
        status = 1;
    }
    editor_document_destroy(doc);

    /* Cleanup. */
    snprintf(path, sizeof(path), "%s/notes.txt", root.canonical_path);
    unlink(path);
    snprintf(path, sizeof(path), "%s/locked.txt", root.canonical_path);
    unlink(path);
    snprintf(path, sizeof(path), "%s/original.txt", root.canonical_path);
    unlink(path);
    unlink(copy_path);
    snprintf(path, sizeof(path), "%s/source.txt", root.canonical_path);
    unlink(path);
    unlink(dest_path);
    snprintf(path, sizeof(path), "%s/readonly_dir/target.txt", root.canonical_path);
    unlink(path);
    rmdir(readonly_dir);
    rmdir(root.canonical_path);

    if (status == 0) {
        printf("editor_document_save_test: in-place save, permission preservation, read-only rejection, Save As "
               "(success/already-exists/invalid-name/outside-workspace), and failed-save-preserves-original all "
               "verified\n");
    }
    return status;
}
