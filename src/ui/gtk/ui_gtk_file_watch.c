#include "ui_gtk_backend.h"
#include "ui_gtk_explorer_internal.h"
#include "ui_gtk_editor_internal.h"

#include <string.h>

/* --- File watching --------------------------------------------------------
 * Watch registration (as directories are loaded, see
 * ui_gtk_file_tree.c's load_row_children()) and application of a
 * FileWatchEvent (an external create/modify/delete/rename detected by a
 * FileWatcher, drained each tick - see ui_gtk_window.c's on_tick) to
 * both the explorer tree and any open editor tab, reusing
 * refresh_row_preserving_expansion() and the editor_handle_external_*
 * functions rather than maintaining a separate tree-mutation path. */

/* Registers a watch on the directory iter itself represents (not its
 * children) with the source's matching FileWatcher, once it's actually
 * been loaded - mirrors FileTree's own "lazy, only what's been
 * expanded" scope exactly: a directory never expanded is never watched.
 * iter's own EXPLORER_COL_PATH is already the directory's root-relative
 * path (both add_files_tree_entry/add_toolkit_tree_row and the two
 * permanent root rows store it that way), so no extra id resolution is
 * needed here.
 *
 * FILES only, deliberately - registering the Toolkit root too surfaced a
 * real, reproducible crash: ExplorerMenuContext (ui_gtk_explorer_menu.c)
 * holds a raw GtkTreeIter across a popup menu's lifetime on the
 * documented assumption that "nothing but user actions ever mutates
 * this tree, and a popup menu grabs input so nothing else can run
 * between menu-open and item-click." A watch-triggered refresh breaks
 * that: the app's own New Folder/Rename/Delete on a watched directory
 * queues a watch event for its own change, and on_tick can end up
 * applying it (tearing down and rebuilding that row's children) during
 * a nested main-loop pump a later, unrelated action triggers
 * (gtk_clipboard_wait_for_text(), a dialog, etc.) - invalidating any
 * GtkTreeIter still pointing at one of those now-removed rows.
 * toolkit_interaction_smoke.c reproduced this ~70% of the time once the
 * Toolkit root was watched. Fixing this properly means auditing every
 * raw-iterator holder against async mutation (ExplorerMenuContext chief
 * among them) - out of this step's scope, so watching stays FILES-only
 * here, matching this step's own test-scope boundary; Toolkit watching
 * (and that audit) is deferred to a later, explicitly-requested pass,
 * the same way Toolkit Sidebar Parity itself was. */
void register_watch_for_loaded_row(GtkBackend *backend, GtkTreeStore *store, GtkTreeIter *iter, int source) {
    if (source != EXPLORER_SOURCE_FILES) {
        return;
    }
    FileWatcher *watcher = backend->file_watcher;
    if (!watcher) {
        return;
    }
    const WorkspaceRoot *root = explorer_root_for_source(backend, source);

    gchar *self_relative = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(store), iter, EXPLORER_COL_PATH, &self_relative, -1);

    char absolute_dir[4096];
    if (self_relative && self_relative[0] == '\0') {
        file_watcher_watch_directory(watcher, root->canonical_path);
    } else if (self_relative && workspace_root_resolve_path(root, self_relative, absolute_dir, sizeof(absolute_dir))) {
        file_watcher_watch_directory(watcher, absolute_dir);
    }
    g_free(self_relative);
}

/* Refreshes the tree row for relative_path's parent directory, if it's
 * currently loaded (a no-op otherwise - nothing to refresh if the user
 * never expanded that far). */
static void refresh_parent_of(GtkBackend *backend, int source, const char *relative_path) {
    GtkTreeStore *store = backend->explorer_store;
    GtkTreeIter root_iter;
    if (!explorer_permanent_root_iter(store, source, &root_iter)) {
        return;
    }

    char parent_relative[4096];
    relative_path_dirname(relative_path, parent_relative, sizeof(parent_relative));

    GtkTreeIter parent_iter;
    if (find_dir_iter_by_relative_path(GTK_TREE_MODEL(store), &root_iter, parent_relative, &parent_iter)) {
        refresh_row_preserving_expansion(backend, GTK_TREE_VIEW(backend->explorer_tree_view), store, &parent_iter);
    }
}

void apply_file_watch_event(GtkBackend *backend, int source, const FileWatchEvent *event) {
    const WorkspaceRoot *root = explorer_root_for_source(backend, source);

    refresh_parent_of(backend, source, event->new_relative_path);
    if (event->type == FILE_WATCH_RENAMED && strcmp(event->old_relative_path, event->new_relative_path) != 0) {
        refresh_parent_of(backend, source, event->old_relative_path);
    }

    switch (event->type) {
        case FILE_WATCH_RENAMED:
            editor_handle_external_rename(backend, root, event->old_relative_path, event->new_relative_path);
            break;
        case FILE_WATCH_DELETED:
            editor_handle_external_delete(backend, root, event->new_relative_path);
            break;
        case FILE_WATCH_MODIFIED:
            editor_handle_external_modification(backend, root, event->new_relative_path);
            break;
        case FILE_WATCH_CREATED:
        default:
            break;
    }
}
/* --- end File watching ----------------------------------------------------- */
