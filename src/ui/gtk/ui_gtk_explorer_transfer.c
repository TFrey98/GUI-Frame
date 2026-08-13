#include "ui_gtk_backend.h"
#include "ui_gtk_explorer_internal.h"
#include "ui_gtk_editor_internal.h"

#include <string.h>
#include <sys/stat.h>

/* --- Explorer clipboard / drag-and-drop ------------------------------------
 * Cut/Copy/Paste (ui_gtk_explorer_menu.c builds the menu items and
 * ui_gtk_explorer_sidebar.c's toolbar Paste button both call into this
 * file) and in-app drag-and-drop (move only). Both ultimately funnel
 * through perform_explorer_move()/perform_explorer_copy() below, so
 * drag-and-drop's own correctness is covered by construction through
 * Cut+Paste's exhaustive test coverage - see the plan file's own note
 * on why a real X11 drag gesture isn't automated here. */

/* Checks whether dest_absolute is src_absolute itself or nested inside
 * it - stops a folder being pasted/dragged into itself or one of its
 * own subfolders. file_copy()'s recursive walk has no such protection
 * on its own (it would recurse without bound); a same-directory move
 * already fails safely at the OS level via rename(), but this keeps
 * failure behavior consistent either way. Boundary-aware, same
 * discipline workspace_root.c's own path_is_contained() uses. */
static gboolean path_would_nest_into_self(const char *src_absolute, const char *dest_absolute) {
    size_t src_len = strlen(src_absolute);
    if (strncmp(dest_absolute, src_absolute, src_len) != 0) {
        return FALSE;
    }
    char next = dest_absolute[src_len];
    return next == '\0' || next == '/';
}

#define PASTE_NAME_MAX_ATTEMPTS 1000

/* Finds a free leaf name for pasting original_name into dest_dir_relative
 * (root-relative, "" meaning dest_root itself) under dest_root: the
 * original name first, then "name (copy)", "name (copy 2)", ... -
 * matching VS Code's own Paste collision convention. Not extension-
 * aware (a deliberate simplification - "script.sh (copy)" over
 * splitting the extension). Writes the chosen leaf name (not a full
 * path) into out_name. */
static gboolean unique_paste_name(const WorkspaceRoot *dest_root, const char *dest_dir_relative,
                                   const char *original_name, char *out_name, size_t out_size) {
    for (int attempt = 0; attempt < PASTE_NAME_MAX_ATTEMPTS; attempt++) {
        if (attempt == 0) {
            snprintf(out_name, out_size, "%s", original_name);
        } else if (attempt == 1) {
            snprintf(out_name, out_size, "%s (copy)", original_name);
        } else {
            snprintf(out_name, out_size, "%s (copy %d)", original_name, attempt);
        }

        char candidate_relative[4096];
        if (dest_dir_relative[0] != '\0') {
            snprintf(candidate_relative, sizeof(candidate_relative), "%s/%s", dest_dir_relative, out_name);
        } else {
            snprintf(candidate_relative, sizeof(candidate_relative), "%s", out_name);
        }

        char resolved[4096];
        if (!workspace_root_resolve_path(dest_root, candidate_relative, resolved, sizeof(resolved))) {
            return FALSE; /* shouldn't happen for a slash-free name under an already-valid directory */
        }
        struct stat st;
        if (lstat(resolved, &st) != 0) {
            return TRUE; /* doesn't exist - this name is free */
        }
    }
    return FALSE;
}

/* Shared validation + destination-name computation for both paste-as-
 * copy and paste-as-move: resolves src/dest_parent, rejects nesting a
 * folder into itself/its own subfolder, and picks a free destination
 * name. On success writes the destination's full relative path (under
 * dest_root) into out_dest_relative. */
static gboolean prepare_paste_destination(GtkBackend *backend, const WorkspaceRoot *src_root,
                                           const char *src_relative_path, const WorkspaceRoot *dest_root,
                                           const char *dest_parent_relative, char *out_dest_relative,
                                           size_t out_size) {
    const char *leaf = strrchr(src_relative_path, '/');
    leaf = leaf ? leaf + 1 : src_relative_path;

    char src_resolved[4096];
    if (!workspace_root_resolve_path(src_root, src_relative_path, src_resolved, sizeof(src_resolved))) {
        show_explorer_error(backend, "Can't paste - the source is missing or outside the workspace.");
        return FALSE;
    }

    char dest_parent_resolved[4096];
    const char *dest_parent_abs;
    if (dest_parent_relative[0] == '\0') {
        dest_parent_abs = dest_root->canonical_path;
    } else if (workspace_root_resolve_path(dest_root, dest_parent_relative, dest_parent_resolved,
                                            sizeof(dest_parent_resolved))) {
        dest_parent_abs = dest_parent_resolved;
    } else {
        show_explorer_error(backend, "Can't paste - the destination is missing or outside the workspace.");
        return FALSE;
    }

    if (path_would_nest_into_self(src_resolved, dest_parent_abs)) {
        show_explorer_error(backend, "Can't paste a folder into itself or one of its own subfolders.");
        return FALSE;
    }

    char dest_name[256];
    if (!unique_paste_name(dest_root, dest_parent_relative, leaf, dest_name, sizeof(dest_name))) {
        show_explorer_error(backend, "Couldn't find a free name for the pasted item.");
        return FALSE;
    }

    if (dest_parent_relative[0] != '\0') {
        snprintf(out_dest_relative, out_size, "%s/%s", dest_parent_relative, dest_name);
    } else {
        snprintf(out_dest_relative, out_size, "%s", dest_name);
    }
    return TRUE;
}

/* Refreshes the explorer row for relative_path (a directory, given
 * directly - not a file whose parent needs computing first, unlike
 * ui_gtk_file_watch.c's own refresh_parent_of), if it's currently
 * loaded. */
static void refresh_explorer_dir(GtkBackend *backend, int source, const char *relative_path) {
    GtkTreeStore *store = backend->explorer_store;
    GtkTreeIter root_iter;
    if (!explorer_permanent_root_iter(store, source, &root_iter)) {
        return;
    }
    GtkTreeIter dir_iter;
    if (find_dir_iter_by_relative_path(GTK_TREE_MODEL(store), &root_iter, relative_path, &dir_iter)) {
        refresh_row_preserving_expansion(backend, GTK_TREE_VIEW(backend->explorer_tree_view), store, &dir_iter);
    }
}

static gboolean perform_explorer_copy(GtkBackend *backend, const WorkspaceRoot *src_root,
                                       const char *src_relative_path, const WorkspaceRoot *dest_root,
                                       int dest_source, const char *dest_parent_relative) {
    char dest_relative[4096];
    if (!prepare_paste_destination(backend, src_root, src_relative_path, dest_root, dest_parent_relative,
                                    dest_relative, sizeof(dest_relative))) {
        return FALSE;
    }

    FileOperationResult result = file_copy(src_root, src_relative_path, dest_root, dest_relative);
    if (result != FILE_OP_OK) {
        show_explorer_error(backend, file_operation_error_message(result));
        return FALSE;
    }

    refresh_explorer_dir(backend, dest_source, dest_parent_relative);
    return TRUE;
}

/* Shared by perform_explorer_paste's CUT branch and drag-and-drop's
 * drop handler. */
static gboolean perform_explorer_move(GtkBackend *backend, const WorkspaceRoot *src_root, int src_source,
                                       const char *src_relative_path, const WorkspaceRoot *dest_root,
                                       int dest_source, const char *dest_parent_relative) {
    char dest_relative[4096];
    if (!prepare_paste_destination(backend, src_root, src_relative_path, dest_root, dest_parent_relative,
                                    dest_relative, sizeof(dest_relative))) {
        return FALSE;
    }

    FileOperationResult result = file_move(src_root, src_relative_path, dest_root, dest_relative);
    if (result != FILE_OP_OK) {
        show_explorer_error(backend, file_operation_error_message(result));
        return FALSE;
    }

    editor_handle_external_move(backend, src_root, src_relative_path, dest_root, dest_relative);

    refresh_explorer_dir(backend, dest_source, dest_parent_relative);
    char src_parent_relative[4096];
    relative_path_dirname(src_relative_path, src_parent_relative, sizeof(src_parent_relative));
    refresh_explorer_dir(backend, src_source, src_parent_relative);
    return TRUE;
}

void explorer_set_clipboard(GtkBackend *backend, ExplorerClipboardMode mode, int source, const char *relative_path) {
    backend->explorer_clipboard.mode = mode;
    backend->explorer_clipboard.source = source;
    snprintf(backend->explorer_clipboard.relative_path, sizeof(backend->explorer_clipboard.relative_path), "%s",
             relative_path ? relative_path : "");
    if (backend->explorer_paste_button) {
        gtk_widget_set_sensitive(backend->explorer_paste_button, mode != EXPLORER_CLIPBOARD_NONE);
    }
}

void perform_explorer_paste(GtkBackend *backend, GtkTreeIter *dest_parent_iter) {
    if (backend->explorer_clipboard.mode == EXPLORER_CLIPBOARD_NONE) {
        return;
    }

    GtkTreeStore *store = backend->explorer_store;
    int dest_source = EXPLORER_SOURCE_FILES;
    gchar *dest_parent_relative = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(store), dest_parent_iter, EXPLORER_COL_SOURCE, &dest_source,
                        EXPLORER_COL_PATH, &dest_parent_relative, -1);
    const WorkspaceRoot *dest_root = explorer_root_for_source(backend, dest_source);
    const char *dest_parent = dest_parent_relative ? dest_parent_relative : "";

    int src_source = backend->explorer_clipboard.source;
    const WorkspaceRoot *src_root = explorer_root_for_source(backend, src_source);
    char src_relative_path[4096];
    snprintf(src_relative_path, sizeof(src_relative_path), "%s", backend->explorer_clipboard.relative_path);
    ExplorerClipboardMode mode = backend->explorer_clipboard.mode;

    if (mode == EXPLORER_CLIPBOARD_CUT) {
        if (perform_explorer_move(backend, src_root, src_source, src_relative_path, dest_root, dest_source,
                                   dest_parent)) {
            explorer_set_clipboard(backend, EXPLORER_CLIPBOARD_NONE, EXPLORER_SOURCE_FILES, "");
        }
    } else {
        perform_explorer_copy(backend, src_root, src_relative_path, dest_root, dest_source, dest_parent);
    }

    g_free(dest_parent_relative);
}

/* Stashes the dragged row's (source, relative_path) on backend - the
 * real source of truth on_explorer_drag_data_received reads back (see
 * EXPLORER_DND_TARGETS's own comment on why nothing needs to actually
 * serialize through GtkSelectionData for a same-app drag). A permanent
 * root (relative_path == "") can never be dragged, same restriction
 * Rename/Delete already have. */
static void on_explorer_drag_begin(GtkWidget *tree_view, GdkDragContext *context, gpointer user_data) {
    (void)context;
    GtkBackend *backend = user_data;
    backend->explorer_drag_active = FALSE;

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        return;
    }

    int source = EXPLORER_SOURCE_FILES;
    gchar *relative_path = NULL;
    gtk_tree_model_get(model, &iter, EXPLORER_COL_SOURCE, &source, EXPLORER_COL_PATH, &relative_path, -1);
    if (relative_path && relative_path[0] != '\0') {
        backend->explorer_drag_active = TRUE;
        backend->explorer_drag_source = source;
        snprintf(backend->explorer_drag_relative_path, sizeof(backend->explorer_drag_relative_path), "%s",
                 relative_path);
    }
    g_free(relative_path);
}

static void on_explorer_drag_data_get(GtkWidget *tree_view, GdkDragContext *context,
                                       GtkSelectionData *selection_data, guint info, guint time,
                                       gpointer user_data) {
    (void)tree_view;
    (void)context;
    (void)info;
    (void)time;
    (void)user_data;
    /* GTK's own DnD protocol expects drag-data-get to actually set
     * something - the real payload lives in backend->explorer_drag_*
     * (stashed in on_explorer_drag_begin), so this placeholder byte just
     * satisfies that expectation for a same-app drag. */
    guchar placeholder = 1;
    gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), 8, &placeholder, 1);
}

static void on_explorer_drag_data_received(GtkWidget *tree_view, GdkDragContext *context, gint x, gint y,
                                            GtkSelectionData *selection_data, guint info, guint time,
                                            gpointer user_data) {
    (void)selection_data;
    (void)info;
    GtkBackend *backend = user_data;

    gboolean handled = FALSE;
    if (backend->explorer_drag_active) {
        GtkTreePath *path = NULL;
        GtkTreeViewDropPosition drop_pos;
        if (gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(tree_view), x, y, &path, &drop_pos)) {
            GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));
            GtkTreeIter dest_iter;
            if (gtk_tree_model_get_iter(model, &dest_iter, path)) {
                gboolean is_dir = FALSE;
                int dest_source = EXPLORER_SOURCE_FILES;
                gtk_tree_model_get(model, &dest_iter, EXPLORER_COL_IS_DIR, &is_dir, EXPLORER_COL_SOURCE,
                                    &dest_source, -1);
                if (is_dir) {
                    const WorkspaceRoot *src_root = explorer_root_for_source(backend, backend->explorer_drag_source);
                    const WorkspaceRoot *dest_root = explorer_root_for_source(backend, dest_source);
                    gchar *dest_parent_relative = NULL;
                    gtk_tree_model_get(model, &dest_iter, EXPLORER_COL_PATH, &dest_parent_relative, -1);
                    handled = perform_explorer_move(backend, src_root, backend->explorer_drag_source,
                                                     backend->explorer_drag_relative_path, dest_root, dest_source,
                                                     dest_parent_relative ? dest_parent_relative : "");
                    g_free(dest_parent_relative);
                } else {
                    show_explorer_error(backend, "Can't drop onto a file - drop it onto a folder instead.");
                }
            }
            gtk_tree_path_free(path);
        }
    }
    backend->explorer_drag_active = FALSE;
    gtk_drag_finish(context, handled, FALSE, time);
}

/* Same-app-only target - source and dest are always this same tree view
 * in this same process, so no real payload needs to cross through
 * GtkSelectionData (see on_explorer_drag_data_get/received). */
static const GtkTargetEntry EXPLORER_DND_TARGETS[] = {
    {"application/x-toolbox-explorer-row", GTK_TARGET_SAME_APP, 0},
};

/* Wires in-app drag-and-drop (move only) onto tree_view - called once
 * from ui_gtk_explorer_sidebar.c's build_explorer_sidebar(). */
void explorer_enable_drag_and_drop(GtkBackend *backend, GtkWidget *tree_view) {
    gtk_tree_view_enable_model_drag_source(GTK_TREE_VIEW(tree_view), GDK_BUTTON1_MASK, EXPLORER_DND_TARGETS,
                                            G_N_ELEMENTS(EXPLORER_DND_TARGETS), GDK_ACTION_MOVE);
    gtk_tree_view_enable_model_drag_dest(GTK_TREE_VIEW(tree_view), EXPLORER_DND_TARGETS,
                                          G_N_ELEMENTS(EXPLORER_DND_TARGETS), GDK_ACTION_MOVE);
    g_signal_connect(tree_view, "drag-begin", G_CALLBACK(on_explorer_drag_begin), backend);
    g_signal_connect(tree_view, "drag-data-get", G_CALLBACK(on_explorer_drag_data_get), backend);
    g_signal_connect(tree_view, "drag-data-received", G_CALLBACK(on_explorer_drag_data_received), backend);
}
/* --- end Explorer clipboard / drag-and-drop --------------------------------- */
