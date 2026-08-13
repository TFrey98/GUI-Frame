#ifndef TOOLBOX_UI_GTK_EDITOR_INTERNAL_H
#define TOOLBOX_UI_GTK_EDITOR_INTERNAL_H

/*
 * Declarations shared across the editor feature: the page widget/Save/
 * Save As/Revert (ui_gtk_editor.c), open-or-focus dedup
 * (ui_gtk_editor_tabs.c), and the watcher-driven external-change/
 * conflict subsystem (ui_gtk_editor_conflicts.c).
 */

#include "ui_gtk_backend.h"

/* Bundles what every Save/Save As/Revert/buffer-changed handler on an
 * editor page needs - built once per page in ui_gtk_editor.c's
 * build_editor_page, but ui_gtk_editor_conflicts.c's conflict dialog
 * also reads it directly (on_conflict_response) - shared for the same
 * reason ui_gtk_explorer_internal.h's ExplorerMenuContext is. */
typedef struct EditorPageContext {
    GtkBackend *backend;
    GtkWidget *page;
} EditorPageContext;

/* --- ui_gtk_editor.c (core: page widget, Save/Save As/Revert) --------- */
GtkWidget *build_editor_page(GtkBackend *backend, Tab *tab);
GtkWidget *build_binary_info_page(GtkBackend *backend, Tab *tab);
/* Extracts page's live buffer text and saves it in place via
 * editor_document_save(); on success refreshes the page's Save/Revert
 * sensitivity + tab label. Shows no dialog itself either way - callers
 * that trigger a single save (the Save button, Ctrl+S, the close/quit
 * confirmation flows) show one via editor_save_error_message() on
 * failure; save_all_modified_editors batches failures into one summary
 * dialog instead of one per file. The one save primitive every other
 * save/close/quit path reuses. */
EditorSaveResult save_editor_page(GtkBackend *backend, GtkWidget *page);
/* Re-reads page's file fresh from disk into its buffer/EditorDocument -
 * the one reload primitive shared by a confirmed Revert click and
 * ui_gtk_editor_conflicts.c's editor_handle_external_modification. */
void reload_editor_document_from_disk(GtkBackend *backend, GtkWidget *page);
/* Saves every writable+modified TAB_TYPE_EDITOR page; reports every
 * failure (not just the first) in one summary dialog - the doc's
 * "Save All reports each failure rather than silently skipping files." */
void save_all_modified_editors(GtkBackend *backend);
/* Back Ctrl+S/Ctrl+Shift+S - resolve the notebook's current page and
 * no-op unless it's a writable TAB_TYPE_EDITOR tab. */
void trigger_save_active_editor(GtkBackend *backend);
void trigger_save_as_active_editor(GtkBackend *backend, GtkWindow *window);

/* --- ui_gtk_editor_tabs.c (open-or-focus dedup) ------------------------ */
/* Finds an already-open editor/binary-info tab for (root, relative_path),
 * skipping any Compare tab - also used directly by
 * ui_gtk_editor_conflicts.c's external-change handlers. */
GtkWidget *find_file_tab(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path);
/* The one entry point ui_gtk_file_tree.c calls for a non-directory row
 * of either source - resolves relative_path against root, classifies,
 * dedups against any already-open tab for the same (root,
 * relative_path) pair, and opens (or silently declines, via
 * show_explorer_error()) accordingly. executable/read_only come from
 * the caller's already-known bits (FileTreeNode for FILES,
 * explorer_toolkit_file_flags() for Toolkit). */
void open_or_focus_file_tab(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path,
                             bool executable, bool read_only);

/* --- ui_gtk_editor_conflicts.c (watcher-driven external changes) ------ */
/* Called from ui_gtk_file_tree.c right after a real (non-create)
 * file_rename() succeeds - updates any open editor/binary-info tab for
 * (root, old_relative_path) (EditorDocument, Tab title, tab label, and
 * the editor page's own header label) so a future Save keeps
 * targeting the right file. A no-op if nothing has that file open. */
void editor_handle_external_rename(GtkBackend *backend, const WorkspaceRoot *root, const char *old_relative_path,
                                    const char *new_relative_path);
/* Generalizes editor_handle_external_rename() to a possibly cross-root
 * move (Cut+Paste, drag-and-drop) - also reassigns doc->root, not just
 * relative_path/display_name/tab title. editor_handle_external_rename()
 * is now a thin same-root wrapper around this. */
void editor_handle_external_move(GtkBackend *backend, const WorkspaceRoot *old_root, const char *old_relative_path,
                                  const WorkspaceRoot *new_root, const char *new_relative_path);
/* Called from apply_file_watch_event() for a FILE_WATCH_DELETED event -
 * a no-op unless (root, relative_path) has an open editor/binary-info
 * tab, in which case it sets doc->deleted_on_disk and shows the "Deleted
 * from disk" banner on that tab. */
void editor_handle_external_delete(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path);
/* Called from apply_file_watch_event() for a FILE_WATCH_MODIFIED event -
 * a no-op unless (root, relative_path) has an open TAB_TYPE_EDITOR tab.
 * Compares a fresh stat() mtime against doc->last_known_mtime to ignore
 * an echo of this app's own save; otherwise reloads silently if the doc
 * has no unsaved changes, or sets doc->externally_modified and shows the
 * Compare/Reload from Disk/Keep Editor Version conflict dialog if it
 * does. */
void editor_handle_external_modification(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path);

#endif /* TOOLBOX_UI_GTK_EDITOR_INTERNAL_H */
