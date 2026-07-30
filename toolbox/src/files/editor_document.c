#include "editor_document.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Monotonic counter, not thread-safe - fine as long as documents are
 * only ever created from the single GTK main-loop thread, same
 * assumption tab_create()'s own counter already makes. */
static DocumentId g_next_document_id = 1;

static const char *basename_of(const char *relative_path) {
    const char *slash = strrchr(relative_path, '/');
    return slash ? slash + 1 : relative_path;
}

static bool leaf_name_is_valid(const char *relative_path) {
    const char *leaf = basename_of(relative_path);
    if (leaf[0] == '\0') {
        return false;
    }
    return strcmp(leaf, ".") != 0 && strcmp(leaf, "..") != 0;
}

EditorDocument *editor_document_open(const WorkspaceRoot *root, const char *relative_path, bool read_only_hint,
                                      bool load_contents) {
    char resolved[4096];
    if (!workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
        return NULL;
    }

    struct stat st;
    if (stat(resolved, &st) != 0) {
        return NULL;
    }

    EditorDocument *doc = calloc(1, sizeof(EditorDocument));
    doc->id = g_next_document_id++;
    snprintf(doc->relative_path, sizeof(doc->relative_path), "%s", relative_path);
    snprintf(doc->display_name, sizeof(doc->display_name), "%s", basename_of(relative_path));
    doc->read_only = read_only_hint;

    if (!load_contents) {
        doc->content_size = (size_t)st.st_size;
        doc->read_only = true;
        return doc;
    }

    FILE *f = fopen(resolved, "rb");
    if (!f) {
        free(doc);
        return NULL;
    }

    size_t to_read = (size_t)st.st_size;
    bool truncated = false;
    if (to_read > EDITOR_DOCUMENT_MAX_SIZE) {
        to_read = EDITOR_DOCUMENT_MAX_SIZE;
        truncated = true;
    }

    doc->contents = malloc(to_read + 1);
    size_t read_count = fread(doc->contents, 1, to_read, f);
    fclose(f);
    doc->contents[read_count] = '\0';
    doc->content_size = read_count;
    if (truncated) {
        doc->read_only = true;
    }

    return doc;
}

void editor_document_destroy(EditorDocument *doc) {
    if (!doc) {
        return;
    }
    free(doc->contents);
    free(doc);
}

EditorSaveResult editor_document_save(const WorkspaceRoot *root, EditorDocument *doc, const char *new_relative_path,
                                       const char *contents, size_t contents_size) {
    if (doc->read_only) {
        return EDITOR_SAVE_READ_ONLY;
    }
    if (new_relative_path && !leaf_name_is_valid(new_relative_path)) {
        return EDITOR_SAVE_INVALID_NAME;
    }

    const char *target_relative = new_relative_path ? new_relative_path : doc->relative_path;
    char resolved[4096];
    if (!workspace_root_resolve_path(root, target_relative, resolved, sizeof(resolved))) {
        return EDITOR_SAVE_OUTSIDE_WORKSPACE;
    }

    struct stat existing_st;
    bool exists = stat(resolved, &existing_st) == 0;
    if (new_relative_path && exists) {
        return EDITOR_SAVE_ALREADY_EXISTS;
    }
    mode_t mode = exists ? (existing_st.st_mode & 0777) : 0644;

    /* Safe write: a temp file beside the destination (same directory,
     * so the final rename() is atomic), flushed+fsynced, chmod'd to
     * match, then renamed over the destination - any failure along
     * the way unlinks the temp file and leaves the original untouched. */
    char tmp_path[4200];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmpXXXXXX", resolved);
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        return EDITOR_SAVE_IO_ERROR;
    }
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(tmp_path);
        return EDITOR_SAVE_IO_ERROR;
    }
    bool write_ok = true;
    if (contents_size > 0) {
        write_ok = fwrite(contents, 1, contents_size, f) == contents_size;
    }
    if (write_ok) {
        write_ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    }
    fclose(f);
    if (!write_ok) {
        unlink(tmp_path);
        return EDITOR_SAVE_IO_ERROR;
    }
    chmod(tmp_path, mode);
    if (rename(tmp_path, resolved) != 0) {
        unlink(tmp_path);
        return EDITOR_SAVE_IO_ERROR;
    }

    char *contents_copy = malloc(contents_size + 1);
    if (contents_size > 0) {
        memcpy(contents_copy, contents, contents_size);
    }
    contents_copy[contents_size] = '\0';
    free(doc->contents);
    doc->contents = contents_copy;
    doc->content_size = contents_size;
    doc->modified = false;

    if (new_relative_path) {
        snprintf(doc->relative_path, sizeof(doc->relative_path), "%s", new_relative_path);
        snprintf(doc->display_name, sizeof(doc->display_name), "%s", basename_of(new_relative_path));
    }

    return EDITOR_SAVE_OK;
}
