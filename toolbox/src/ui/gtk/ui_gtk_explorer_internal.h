#ifndef TOOLBOX_UI_GTK_EXPLORER_INTERNAL_H
#define TOOLBOX_UI_GTK_EXPLORER_INTERNAL_H

/*
 * Declarations shared across the explorer sidebar feature: tree-store
 * population/lazy-loading (ui_gtk_file_tree.c), the sidebar widget/
 * toolbar/inline create-rename (ui_gtk_explorer_sidebar.c), file
 * watching (ui_gtk_file_watch.c), Cut/Copy/Paste/drag-and-drop
 * (ui_gtk_explorer_transfer.c), Reveal in Explorer
 * (ui_gtk_explorer_navigation.c), the context menu
 * (ui_gtk_explorer_menu.c), and the Properties dialog
 * (ui_gtk_explorer_dialogs.c).
 */

#include "ui_gtk_backend.h"

/* Explorer sidebar tree columns - ui_gtk_file_tree.c builds/syncs this
 * store; ui_gtk_explorer_menu.c's context menu (and
 * ui_gtk_explorer_dialogs.c's Properties dialog) read these same
 * columns. */
enum {
    EXPLORER_COL_ICON,
    EXPLORER_COL_NAME,
    EXPLORER_COL_PATH, /* root-relative for either source - see explorer_root_for_source() */
    EXPLORER_COL_IS_DIR,
    EXPLORER_COL_LOADED,
    EXPLORER_COL_SOURCE,
    EXPLORER_COL_NODE_ID, /* FileNodeId - FILES rows only, unused for TOOLKIT rows */
    EXPLORER_COL_COUNT
};

/* GTK-layer-only sentinel (never a real FileNodeId - those start at 1
 * and only ever increase) marking a blank row being named for a
 * pending New File/New Folder, so the shared "edited" handler can tell
 * a create from a rename. */
#define EXPLORER_PENDING_CREATE_ID ((guint64)0xFFFFFFFFFFFFFFFFULL)

/* Built by ui_gtk_explorer_menu.c's popup_explorer_context_menu and
 * read by its own item handlers *and*
 * ui_gtk_explorer_dialogs.c's on_explorer_menu_properties - the one
 * cross-file case, since Properties otherwise lives in its own file
 * (see the restructuring plan's splitting rule). Safe to trust directly
 * (unlike the object panel's file-local MenuItemContext, which
 * re-checks live state): nothing but user actions ever mutates this
 * tree, and a popup menu grabs input so nothing else can run between
 * menu-open and item-click. */
typedef struct ExplorerMenuContext {
    GtkBackend *backend;
    GtkTreeIter iter; /* the row the menu was built for */
} ExplorerMenuContext;

/* --- ui_gtk_file_tree.c (core: tree-store population, lazy loading, refresh) --- */
void load_row_children(GtkBackend *backend, GtkTreeStore *store, GtkTreeIter *iter);
/* Wired as the "row-expanded" signal handler by
 * ui_gtk_explorer_sidebar.c's build_explorer_sidebar(). */
void on_explorer_row_expanded(GtkTreeView *tree_view, GtkTreeIter *iter, GtkTreePath *tree_path, gpointer user_data);
void refresh_row_preserving_expansion(GtkBackend *backend, GtkTreeView *tree_view, GtkTreeStore *store,
                                       GtkTreeIter *iter);
/* Picks the right WorkspaceRoot for a row's EXPLORER_COL_SOURCE value -
 * every explorer action that used to hardcode
 * workbench_get_file_workspace_root() goes through this instead, so
 * the same logic works for either source unchanged. */
const WorkspaceRoot *explorer_root_for_source(GtkBackend *backend, int source);
/* FileTreeNode already caches executable/read_only (lstat at scan
 * time); ToolkitEntry has neither field. Computes both fresh via
 * stat()+access(W_OK) - identical logic to file_tree.c's own
 * classify_entry(), just not cached anywhere for Toolkit. */
void explorer_toolkit_file_flags(const char *absolute_path, bool *out_executable, bool *out_read_only);
/* Finds the GtkTreeIter under scope (searched recursively, matching
 * each row's own EXPLORER_COL_PATH directly) whose root-relative path
 * equals relative_path - "" means scope itself. Only ever needs to
 * succeed as far as an already-loaded directory goes. Promoted from
 * static (Step 6) for reuse by perform_explorer_paste's source-parent
 * refresh. */
gboolean find_dir_iter_by_relative_path(GtkTreeModel *model, GtkTreeIter *scope, const char *relative_path,
                                         GtkTreeIter *out);
/* Recursively searches scope for a row whose EXPLORER_COL_NAME matches
 * name exactly (one segment, not a full path) - used by
 * refresh_row_preserving_expansion (this file) and
 * ui_gtk_explorer_navigation.c's reveal_in_explorer(). */
gboolean explorer_find_child_by_name(GtkTreeModel *model, GtkTreeIter *parent, const char *name, GtkTreeIter *out);
/* The merged explorer_store has exactly two top-level rows (TOOLBOX,
 * Toolkit); finds the one matching source - used by
 * ui_gtk_file_watch.c, ui_gtk_explorer_transfer.c, and
 * ui_gtk_explorer_navigation.c. */
gboolean explorer_permanent_root_iter(GtkTreeStore *store, int source, GtkTreeIter *out);
/* Strips the final path component, leaving the parent directory's
 * root-relative path - used by ui_gtk_file_watch.c's refresh_parent_of()
 * and ui_gtk_explorer_transfer.c's perform_explorer_move(). */
void relative_path_dirname(const char *relative_path, char *out, size_t out_size);

/* --- ui_gtk_explorer_sidebar.c (widget construction, toolbar, inline create/rename) --- */
GtkWidget *build_explorer_sidebar(GtkBackend *backend);
void start_new_entry(GtkBackend *backend, GtkTreeIter *parent_iter, gboolean is_folder);

/* --- ui_gtk_file_watch.c (watch registration + FileWatchEvent application) --- */
/* Called from load_row_children() (ui_gtk_file_tree.c) right after a
 * successful scan, for either source - a no-op unless source is
 * EXPLORER_SOURCE_FILES (see this function's own definition for why
 * Toolkit watching is deliberately out of scope). */
void register_watch_for_loaded_row(GtkBackend *backend, GtkTreeStore *store, GtkTreeIter *iter, int source);
/* Applies one FileWatchEvent (drained from backend->file_watcher/
 * toolkit_watcher, source picking which) to the explorer tree and any
 * open editor tab - called from ui_gtk_window.c's on_tick. */
void apply_file_watch_event(GtkBackend *backend, int source, const FileWatchEvent *event);

/* --- ui_gtk_explorer_transfer.c (Cut/Copy/Paste, move, drag-and-drop) --- */
/* Sets backend->explorer_clipboard and enables the toolbar Paste
 * button - the one entry point both Cut and Copy
 * (ui_gtk_explorer_menu.c) call. */
void explorer_set_clipboard(GtkBackend *backend, ExplorerClipboardMode mode, int source, const char *relative_path);
/* Pastes the clipboard's item into dest_parent_iter's directory - the
 * one entry point both the toolbar Paste button and the context menu's
 * Paste item call. A no-op (shows an error) if the clipboard is empty,
 * the paste would put a folder inside itself/its own subfolder, or the
 * underlying file_copy()/file_move() fails. CUT mode clears the
 * clipboard on success (a cut is spent after one paste). */
void perform_explorer_paste(GtkBackend *backend, GtkTreeIter *dest_parent_iter);
/* Wires in-app drag-and-drop (move only) onto tree_view - called once
 * from ui_gtk_explorer_sidebar.c's build_explorer_sidebar(). */
void explorer_enable_drag_and_drop(GtkBackend *backend, GtkWidget *tree_view);

/* --- ui_gtk_explorer_navigation.c (Reveal in Explorer) --- */
/* VS Code's own "Reveal in Explorer" - progressively loads and expands
 * every ancestor directory down to relative_path (root-relative, ""
 * meaning the permanent root itself), then selects and scrolls it into
 * view. Backs a search result's folder activation. A no-op if
 * relative_path doesn't resolve to a real row (e.g. deleted since the
 * search ran). */
void reveal_in_explorer(GtkBackend *backend, int source, const char *relative_path);

/* --- ui_gtk_explorer_menu.c ----------------------------------------------- */
void popup_explorer_context_menu(GtkBackend *backend, GtkWidget *tree_view, GtkTreePath *path,
                                  GdkEventButton *event);
gboolean on_explorer_button_press(GtkWidget *tree_view, GdkEventButton *event, gpointer user_data);
gboolean on_explorer_popup_menu(GtkWidget *tree_view, gpointer user_data);

/* --- ui_gtk_explorer_dialogs.c (Properties dialog) --------------------- */
/* Properties is triggered from the explorer's context menu
 * (ui_gtk_explorer_menu.c builds the menu item), but its entire effect
 * is unconditionally showing a dialog, so its definition lives in its
 * own file - see the restructuring plan's splitting rule. */
void on_explorer_menu_properties(GtkMenuItem *item, gpointer user_data);

#endif /* TOOLBOX_UI_GTK_EXPLORER_INTERNAL_H */
