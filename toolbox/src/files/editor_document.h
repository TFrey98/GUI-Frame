#ifndef TOOLBOX_EDITOR_DOCUMENT_H
#define TOOLBOX_EDITOR_DOCUMENT_H

#include <stdbool.h>
#include <stddef.h>

#include "file_ids.h"
#include "workspace_root.h"

/* Large-file threshold - beyond this, editor_document_open() loads
 * only the first EDITOR_DOCUMENT_MAX_SIZE bytes and forces read_only
 * true regardless of the caller's own hint ("large files trigger a
 * warning or restricted mode" - restricted mode, here). The GTK layer
 * detects this after the fact by comparing content_size against this
 * same constant, so no extra struct field is needed. */
#define EDITOR_DOCUMENT_MAX_SIZE (2 * 1024 * 1024)

typedef struct EditorDocument {
    DocumentId id;

    char relative_path[4096];
    char display_name[256];

    char *contents;      /* NUL-terminated; NULL when not loaded (binary target) */
    size_t content_size;

    bool modified;
    bool read_only;
    bool externally_modified; /* unused until Step 6's file watcher exists */
    bool deleted_on_disk;     /* unused until Step 6's file watcher exists */

    int cursor_line;         /* unused until Step 10's session persistence */
    int cursor_column;       /* unused until Step 10's session persistence */
    int first_visible_line;  /* unused until Step 10's session persistence */
} EditorDocument;

/*
 * Resolves relative_path through root and reads it. load_contents
 * false (a binary file, per file_classify()) only stat()s the file for
 * content_size/display_name - contents stays NULL, read_only forced
 * true. load_contents true beyond EDITOR_DOCUMENT_MAX_SIZE loads only
 * that much and forces read_only true regardless of read_only_hint.
 * Returns NULL if relative_path can't be resolved or read.
 */
EditorDocument *editor_document_open(const WorkspaceRoot *root, const char *relative_path, bool read_only_hint,
                                      bool load_contents);
void editor_document_destroy(EditorDocument *doc);

typedef enum EditorSaveResult {
    EDITOR_SAVE_OK,
    EDITOR_SAVE_OUTSIDE_WORKSPACE,
    EDITOR_SAVE_INVALID_NAME,
    EDITOR_SAVE_ALREADY_EXISTS, /* Save As only - destination already exists */
    EDITOR_SAVE_READ_ONLY,
    EDITOR_SAVE_IO_ERROR
} EditorSaveResult;

/*
 * Writes contents (contents_size bytes) to disk via a safe write: a
 * temp file created beside the destination (same directory, so the
 * final rename is atomic), flushed and fsynced, chmod'd to match the
 * destination's existing permissions (or 0644 for a brand-new file),
 * then renamed over the destination - any failure along the way
 * unlinks the temp file and leaves the original completely untouched.
 * Rejected outright with EDITOR_SAVE_READ_ONLY if doc->read_only.
 *
 * new_relative_path NULL means "save doc back to its own
 * relative_path" (plain Save). Non-NULL means Save As - the leaf name
 * is validated (empty/"."/".." rejected), an existing destination is
 * rejected with EDITOR_SAVE_ALREADY_EXISTS (no overwrite), and the
 * *old* file at doc's previous relative_path is left untouched. On
 * success, doc->contents/content_size are replaced with a copy of
 * contents, doc->modified is cleared, and - for Save As - doc's
 * relative_path/display_name switch to new_relative_path. On any
 * failure doc is left completely unchanged.
 */
EditorSaveResult editor_document_save(const WorkspaceRoot *root, EditorDocument *doc, const char *new_relative_path,
                                       const char *contents, size_t contents_size);

#endif /* TOOLBOX_EDITOR_DOCUMENT_H */
