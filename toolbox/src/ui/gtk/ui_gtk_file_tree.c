#include "ui_gtk_internal.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- Explorer sidebar ---------------------------------------------------
 * One shared tree merging two independent sources under permanent,
 * always-present root rows: "TOOLBOX" (FileTree, backed by the
 * WorkspaceRoot's files/ directory) and "Toolkit" (toolkit_index's
 * toolkit/ directory - pre-existing). Each root and every directory
 * beneath it is lazily expanded on demand, same behavior for both
 * sources, dispatched by EXPLORER_COL_SOURCE (see ui_gtk_internal.h for
 * the column/source enums and the pending-create sentinel). */

/* Picks the right WorkspaceRoot for a row's EXPLORER_COL_SOURCE value -
 * every explorer action that used to hardcode
 * workbench_get_file_workspace_root() now goes through this instead,
 * so the exact same logic (menus, create/rename/delete, open-as-text,
 * terminal actions, copy path) works for either source unchanged. */
const WorkspaceRoot *explorer_root_for_source(GtkBackend *backend, int source) {
    return source == EXPLORER_SOURCE_TOOLKIT ? workbench_get_toolkit_workspace_root(backend->workbench)
                                              : workbench_get_file_workspace_root(backend->workbench);
}

/* FileTreeNode already caches executable/read_only (lstat at scan
 * time, see file_tree.c's classify_entry()); ToolkitEntry has neither
 * field. Computes both fresh, identical logic, for a Toolkit row. */
void explorer_toolkit_file_flags(const char *absolute_path, bool *out_executable, bool *out_read_only) {
    struct stat st;
    if (stat(absolute_path, &st) == 0) {
        *out_executable = (st.st_mode & S_IXUSR) != 0;
        *out_read_only = access(absolute_path, W_OK) != 0;
    } else {
        *out_executable = false;
        *out_read_only = true;
    }
}

static const char *icon_for_file_node_type(FileNodeType type) {
    switch (type) {
        case FILE_NODE_DIRECTORY:
            return "folder";
        case FILE_NODE_SYMLINK:
            return "emblem-symbolic-link";
        case FILE_NODE_OTHER:
            return "dialog-question";
        case FILE_NODE_REGULAR:
        default:
            return "text-x-generic";
    }
}

/* Appends node as a row under parent. Directories get a single
 * placeholder child so their expander arrow shows immediately; the
 * placeholder is replaced with real children lazily in
 * load_row_children, once the row is actually expanded. A symlink
 * never gets a placeholder, even if its target is a directory - see
 * file_tree.h's own comment on why this step never follows one. */
static void add_files_tree_entry(GtkTreeStore *store, GtkTreeIter *parent, const FileTreeNode *node) {
    GtkTreeIter iter;
    gtk_tree_store_append(store, &iter, parent);
    gtk_tree_store_set(store, &iter,
        EXPLORER_COL_ICON, icon_for_file_node_type(node->type),
        EXPLORER_COL_NAME, node->name,
        EXPLORER_COL_PATH, node->relative_path,
        EXPLORER_COL_IS_DIR, node->type == FILE_NODE_DIRECTORY,
        EXPLORER_COL_LOADED, FALSE,
        EXPLORER_COL_SOURCE, EXPLORER_SOURCE_FILES,
        EXPLORER_COL_NODE_ID, (guint64)node->id,
        -1);

    if (node->type == FILE_NODE_DIRECTORY) {
        GtkTreeIter placeholder;
        gtk_tree_store_append(store, &placeholder, &iter);
        gtk_tree_store_set(store, &placeholder, EXPLORER_COL_NAME, "", -1);
    }
}

/* Strips toolkit_root's own canonical_path prefix (+1 for the '/') from
 * an absolute path, so Toolkit rows store a root-relative path just
 * like FILES rows do - every safety primitive downstream
 * (workspace_root_resolve_path, file_operations, editor_document_open)
 * requires a relative path, and ToolkitEntry only ever hands back
 * absolute ones. */
static const char *toolkit_relative_from_absolute(const WorkspaceRoot *toolkit_root, const char *absolute_path) {
    size_t root_len = strlen(toolkit_root->canonical_path);
    if (strncmp(absolute_path, toolkit_root->canonical_path, root_len) != 0) {
        return absolute_path; /* shouldn't happen - toolkit_scan_directory() never escapes its own dir */
    }
    const char *rest = absolute_path + root_len;
    return *rest == '/' ? rest + 1 : rest;
}

/* Same shape as add_files_tree_entry, for a Toolkit-sourced row. */
static void add_toolkit_tree_row(const WorkspaceRoot *toolkit_root, GtkTreeStore *store, GtkTreeIter *parent,
                                  const ToolkitEntry *entry) {
    GtkTreeIter iter;
    gtk_tree_store_append(store, &iter, parent);
    gtk_tree_store_set(store, &iter,
        EXPLORER_COL_ICON, entry->is_dir ? "folder" : "text-x-generic",
        EXPLORER_COL_NAME, entry->name,
        EXPLORER_COL_PATH, toolkit_relative_from_absolute(toolkit_root, entry->path),
        EXPLORER_COL_IS_DIR, entry->is_dir,
        EXPLORER_COL_LOADED, FALSE,
        EXPLORER_COL_SOURCE, EXPLORER_SOURCE_TOOLKIT,
        EXPLORER_COL_NODE_ID, (guint64)0,
        -1);

    if (entry->is_dir) {
        GtkTreeIter placeholder;
        gtk_tree_store_append(store, &placeholder, &iter);
        gtk_tree_store_set(store, &placeholder, EXPLORER_COL_NAME, "", -1);
    }
}

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
 * real, reproducible crash: ExplorerMenuContext (ui_gtk_menus.c) holds a
 * raw GtkTreeIter across a popup menu's lifetime on the documented
 * assumption that "nothing but user actions ever mutates this tree, and
 * a popup menu grabs input so nothing else can run between menu-open and
 * item-click." A watch-triggered refresh breaks that: the app's own
 * New Folder/Rename/Delete on a watched directory queues a watch event
 * for its own change, and on_tick can end up applying it (tearing down
 * and rebuilding that row's children) during a nested main-loop pump
 * a later, unrelated action triggers (gtk_clipboard_wait_for_text(), a
 * dialog, etc.) - invalidating any GtkTreeIter still pointing at one of
 * those now-removed rows. toolkit_interaction_smoke.c reproduced this
 * ~70% of the time once the Toolkit root was watched. Fixing this
 * properly means auditing every raw-iterator holder against async
 * mutation (ExplorerMenuContext chief among them) - out of this step's
 * scope, so watching stays FILES-only here, matching this step's own
 * test-scope boundary; Toolkit watching (and that audit) is deferred to
 * a later, explicitly-requested pass, the same way Toolkit Sidebar
 * Parity itself was. */
static void register_watch_for_loaded_row(GtkBackend *backend, GtkTreeStore *store, GtkTreeIter *iter, int source) {
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

/* (Re)loads iter's children from whichever source it belongs to -
 * FILES via file_tree_load_children()/file_tree_get_child_at(),
 * TOOLKIT via the same toolkit_scan_directory() call the sidebar always
 * used. Safe to call whether iter currently holds a lazy placeholder
 * (first expansion) or a full previous listing (refresh/re-expansion):
 * new rows are appended *before* the old ones are removed, so the row's
 * child count never passes through zero mid-transition - GtkTreeView
 * would otherwise auto-collapse a row that's actively being
 * expanded/refreshed. */
void load_row_children(GtkBackend *backend, GtkTreeStore *store, GtkTreeIter *iter) {
    int old_count = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store), iter);

    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(GTK_TREE_MODEL(store), iter, EXPLORER_COL_SOURCE, &source, -1);

    if (source == EXPLORER_SOURCE_FILES) {
        guint64 node_id = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(store), iter, EXPLORER_COL_NODE_ID, &node_id, -1);
        int n = file_tree_load_children(backend->file_tree, (FileNodeId)node_id);
        for (int i = 0; i < n; i++) {
            const FileTreeNode *child = file_tree_get_child_at(backend->file_tree, (FileNodeId)node_id, i);
            add_files_tree_entry(store, iter, child);
        }
    } else {
        const WorkspaceRoot *toolkit_root = workbench_get_toolkit_workspace_root(backend->workbench);
        gchar *relative_path = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(store), iter, EXPLORER_COL_PATH, &relative_path, -1);

        /* EXPLORER_COL_PATH is root-relative (empty string means the
         * Toolkit root itself) - toolkit_scan_directory() still needs a
         * real absolute directory, same empty-string-means-root special
         * case open_terminal_at() already established since
         * workspace_root_resolve_path() rejects an empty string outright. */
        char resolved[4096];
        const char *absolute_dir = NULL;
        if (relative_path && relative_path[0] == '\0') {
            absolute_dir = toolkit_root->canonical_path;
        } else if (relative_path && workspace_root_resolve_path(toolkit_root, relative_path, resolved,
                                                                  sizeof(resolved))) {
            absolute_dir = resolved;
        }

        if (absolute_dir) {
            ToolkitEntry entries[TOOLKIT_INDEX_MAX_ENTRIES];
            int n = toolkit_scan_directory(absolute_dir, entries, TOOLKIT_INDEX_MAX_ENTRIES);
            for (int i = 0; i < n; i++) {
                add_toolkit_tree_row(toolkit_root, store, iter, &entries[i]);
                free(entries[i].name);
                free(entries[i].path);
            }
        }
        g_free(relative_path);
    }

    for (int i = 0; i < old_count; i++) {
        GtkTreeIter old_child;
        if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(store), &old_child, iter, 0)) {
            gtk_tree_store_remove(store, &old_child);
        }
    }

    gtk_tree_store_set(store, iter, EXPLORER_COL_LOADED, TRUE, -1);
    register_watch_for_loaded_row(backend, store, iter, source);
}

/* Lazily loads a row's contents the first time it's expanded; re-
 * expanding after a collapse reuses what was already loaded rather than
 * re-scanning. */
static void on_explorer_row_expanded(GtkTreeView *tree_view, GtkTreeIter *iter, GtkTreePath *tree_path,
                                      gpointer user_data) {
    (void)tree_path;
    GtkBackend *backend = user_data;
    GtkTreeStore *store = GTK_TREE_STORE(gtk_tree_view_get_model(tree_view));

    gboolean loaded = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(store), iter, EXPLORER_COL_LOADED, &loaded, -1);
    if (loaded) {
        return;
    }

    load_row_children(backend, store, iter);
}

static gboolean explorer_find_child_by_name(GtkTreeModel *model, GtkTreeIter *parent, const char *name,
                                             GtkTreeIter *out) {
    GtkTreeIter child;
    if (!gtk_tree_model_iter_children(model, &child, parent)) {
        return FALSE;
    }
    do {
        gchar *child_name = NULL;
        gtk_tree_model_get(model, &child, EXPLORER_COL_NAME, &child_name, -1);
        gboolean match = child_name && strcmp(child_name, name) == 0;
        g_free(child_name);
        if (match) {
            *out = child;
            return TRUE;
        }
    } while (gtk_tree_model_iter_next(model, &child));
    return FALSE;
}

/* Refreshes iter's own children, then - the checkpoint beyond a flat
 * reload - re-expands and recursively refreshes any new child whose
 * name matches one that was expanded *before* the reload ("preserves
 * expanded folders where possible"; ids aren't stable across a reload,
 * by FileTree's own design, so name is the only thing to match on).
 * Loads a matched child's contents directly rather than expanding it
 * first, which would trigger a redundant second scan through
 * on_explorer_row_expanded's own "row-expanded" handling. */
void refresh_row_preserving_expansion(GtkBackend *backend, GtkTreeView *tree_view, GtkTreeStore *store,
                                       GtkTreeIter *iter) {
    GPtrArray *expanded_names = g_ptr_array_new_with_free_func(g_free);
    GtkTreeIter child;
    gboolean has_child = gtk_tree_model_iter_children(GTK_TREE_MODEL(store), &child, iter);
    while (has_child) {
        GtkTreePath *child_path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &child);
        gboolean expanded = gtk_tree_view_row_expanded(tree_view, child_path);
        gtk_tree_path_free(child_path);
        if (expanded) {
            gchar *name = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(store), &child, EXPLORER_COL_NAME, &name, -1);
            if (name) {
                g_ptr_array_add(expanded_names, name);
            }
        }
        has_child = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &child);
    }

    load_row_children(backend, store, iter);

    for (guint i = 0; i < expanded_names->len; i++) {
        const char *name = g_ptr_array_index(expanded_names, i);
        GtkTreeIter new_child;
        if (explorer_find_child_by_name(GTK_TREE_MODEL(store), iter, name, &new_child)) {
            refresh_row_preserving_expansion(backend, tree_view, store, &new_child);
            GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &new_child);
            gtk_tree_view_expand_row(tree_view, path, FALSE);
            gtk_tree_path_free(path);
        }
    }

    g_ptr_array_free(expanded_names, TRUE);
}

/* Re-scans both permanent roots, replacing their current contents -
 * was a flat Toolkit-only reload before this step; now recursively
 * preserves expansion state too (see refresh_row_preserving_expansion). */
static void on_explorer_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkBackend *backend = user_data;
    GtkTreeStore *store = backend->explorer_store;
    GtkTreeView *tree_view = GTK_TREE_VIEW(backend->explorer_tree_view);

    GtkTreeIter root_iter;
    gboolean has = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &root_iter);
    while (has) {
        refresh_row_preserving_expansion(backend, tree_view, store, &root_iter);
        has = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &root_iter);
    }
}

/* Double-click (or Enter) on a row toggles a directory row - unchanged
 * for either source. A non-directory row of *either* source instead
 * opens (or focuses) an editor/binary-info tab via
 * open_or_focus_file_tab() - FILES supplies its already-cached
 * FileTreeNode bits, Toolkit stats them fresh via
 * explorer_toolkit_file_flags() since ToolkitEntry tracks neither. */
static void on_explorer_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column,
                                       gpointer user_data) {
    (void)column;
    GtkBackend *backend = user_data;
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
        return;
    }

    gboolean is_dir = FALSE;
    int source = EXPLORER_SOURCE_FILES;
    guint64 node_id = 0;
    gchar *relative_path = NULL;
    gtk_tree_model_get(model, &iter, EXPLORER_COL_IS_DIR, &is_dir, EXPLORER_COL_SOURCE, &source,
                        EXPLORER_COL_NODE_ID, &node_id, EXPLORER_COL_PATH, &relative_path, -1);

    if (is_dir) {
        if (gtk_tree_view_row_expanded(tree_view, path)) {
            gtk_tree_view_collapse_row(tree_view, path);
        } else {
            gtk_tree_view_expand_row(tree_view, path, FALSE);
        }
        g_free(relative_path);
        return;
    }

    const WorkspaceRoot *root = explorer_root_for_source(backend, source);
    if (source == EXPLORER_SOURCE_FILES) {
        const FileTreeNode *node = file_tree_find(backend->file_tree, (FileNodeId)node_id);
        if (node) {
            open_or_focus_file_tab(backend, root, node->relative_path, node->executable, node->read_only);
        }
    } else if (relative_path) {
        char resolved[4096];
        bool executable = false, read_only = true;
        if (workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
            explorer_toolkit_file_flags(resolved, &executable, &read_only);
        }
        open_or_focus_file_tab(backend, root, relative_path, executable, read_only);
    }
    g_free(relative_path);
}

/* Appends one blank placeholder row under parent_iter (loading/
 * expanding it first if it was still an unloaded lazy folder) and
 * starts inline-editing it - the shared "edited"/"editing-canceled"
 * handlers below do the actual file_create()/directory_create() once
 * the user commits a name, or discard the row on cancel. */
void start_new_entry(GtkBackend *backend, GtkTreeIter *parent_iter, gboolean is_folder) {
    GtkTreeStore *store = backend->explorer_store;
    GtkTreeView *tree_view = GTK_TREE_VIEW(backend->explorer_tree_view);

    gboolean loaded = FALSE;
    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(GTK_TREE_MODEL(store), parent_iter, EXPLORER_COL_LOADED, &loaded, EXPLORER_COL_SOURCE,
                        &source, -1);
    if (!loaded) {
        load_row_children(backend, store, parent_iter);
    }

    GtkTreeIter new_iter;
    gtk_tree_store_append(store, &new_iter, parent_iter);
    gtk_tree_store_set(store, &new_iter,
        EXPLORER_COL_ICON, is_folder ? "folder" : "text-x-generic",
        EXPLORER_COL_NAME, "",
        EXPLORER_COL_PATH, "",
        EXPLORER_COL_IS_DIR, is_folder,
        EXPLORER_COL_LOADED, TRUE,
        EXPLORER_COL_SOURCE, source, /* inherits parent's source - a new row under Toolkit stays Toolkit-sourced */
        EXPLORER_COL_NODE_ID, EXPLORER_PENDING_CREATE_ID,
        -1);

    /* Expand parent *after* the new row exists, not before - an empty
     * folder has zero children right up until the append above, and
     * gtk_tree_view_expand_row() on a childless row silently does
     * nothing (there's nothing to expand), leaving the row this is
     * about to start editing invisible. */
    GtkTreePath *parent_path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), parent_iter);
    gtk_tree_view_expand_row(tree_view, parent_path, FALSE);
    gtk_tree_path_free(parent_path);

    GtkTreePath *new_path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &new_iter);
    if (backend->explorer_editing_row) {
        gtk_tree_row_reference_free(backend->explorer_editing_row);
    }
    backend->explorer_editing_row = gtk_tree_row_reference_new(GTK_TREE_MODEL(store), new_path);
    g_object_set(backend->explorer_name_renderer, "editable", TRUE, NULL);
    gtk_tree_view_set_cursor_on_cell(tree_view, new_path, gtk_tree_view_get_column(tree_view, 0),
                                      backend->explorer_name_renderer, TRUE);
    gtk_tree_path_free(new_path);
}

/* A selected directory row (either source) is the target directly; a
 * selected file row targets its parent; nothing selected defaults to
 * the TOOLBOX root - same convention VS Code's own toolbar New File/
 * Folder uses. */
static void explorer_target_parent_from_selection(GtkBackend *backend, GtkTreeIter *out_parent_iter) {
    GtkTreeView *tree_view = GTK_TREE_VIEW(backend->explorer_tree_view);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(tree_view);
    GtkTreeModel *model;
    GtkTreeIter selected;
    if (gtk_tree_selection_get_selected(selection, &model, &selected)) {
        gboolean is_dir = FALSE;
        gtk_tree_model_get(model, &selected, EXPLORER_COL_IS_DIR, &is_dir, -1);
        if (is_dir) {
            *out_parent_iter = selected;
            return;
        }
        GtkTreeIter parent;
        if (gtk_tree_model_iter_parent(model, &parent, &selected)) {
            *out_parent_iter = parent;
            return;
        }
    }
    gtk_tree_model_get_iter_first(GTK_TREE_MODEL(backend->explorer_store), out_parent_iter);
}

static void on_explorer_new_file_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkBackend *backend = user_data;
    GtkTreeIter parent_iter;
    explorer_target_parent_from_selection(backend, &parent_iter);
    start_new_entry(backend, &parent_iter, FALSE);
}

static void on_explorer_new_folder_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkBackend *backend = user_data;
    GtkTreeIter parent_iter;
    explorer_target_parent_from_selection(backend, &parent_iter);
    start_new_entry(backend, &parent_iter, TRUE);
}

static void on_explorer_collapse_all_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkBackend *backend = user_data;
    gtk_tree_view_collapse_all(GTK_TREE_VIEW(backend->explorer_tree_view));
}

/* Shared by every inline edit (both a pending New File/Folder commit
 * and a Rename commit) - GTK's own cell editing already gives Escape-
 * cancels/Enter-commits for free, so this only needs to handle the
 * commit side. Always ends by reloading the parent, which reflects
 * real disk state either way: a failed/blank commit just discards the
 * pending row or leaves a rename's original name in place, with no
 * separate "undo" logic needed. */
static void on_explorer_name_edited(GtkCellRendererText *renderer, gchar *path_str, gchar *new_text,
                                     gpointer user_data) {
    (void)renderer;
    (void)path_str;
    GtkBackend *backend = user_data;
    g_object_set(backend->explorer_name_renderer, "editable", FALSE, NULL);

    if (!backend->explorer_editing_row || !gtk_tree_row_reference_valid(backend->explorer_editing_row)) {
        return;
    }
    GtkTreeStore *store = backend->explorer_store;
    GtkTreePath *row_path = gtk_tree_row_reference_get_path(backend->explorer_editing_row);
    GtkTreeIter iter;
    gboolean has_iter = gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter, row_path);
    gtk_tree_path_free(row_path);
    gtk_tree_row_reference_free(backend->explorer_editing_row);
    backend->explorer_editing_row = NULL;
    if (!has_iter) {
        return;
    }

    GtkTreeIter parent_iter;
    if (!gtk_tree_model_iter_parent(GTK_TREE_MODEL(store), &parent_iter, &iter)) {
        return; /* a permanent root - never offered rename/create-into, stay defensive */
    }

    gchar *trimmed = g_strdup(new_text);
    g_strstrip(trimmed);

    if (trimmed[0] != '\0' && !strchr(trimmed, '/')) {
        guint64 node_id = 0;
        gboolean is_dir = FALSE;
        int source = EXPLORER_SOURCE_FILES;
        gchar *old_relative_path = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, EXPLORER_COL_NODE_ID, &node_id, EXPLORER_COL_IS_DIR,
                            &is_dir, EXPLORER_COL_SOURCE, &source, EXPLORER_COL_PATH, &old_relative_path, -1);
        gchar *parent_relative_path = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &parent_iter, EXPLORER_COL_PATH, &parent_relative_path, -1);

        char new_relative[4096];
        if (parent_relative_path && parent_relative_path[0] != '\0') {
            snprintf(new_relative, sizeof(new_relative), "%s/%s", parent_relative_path, trimmed);
        } else {
            snprintf(new_relative, sizeof(new_relative), "%s", trimmed);
        }

        const WorkspaceRoot *root = explorer_root_for_source(backend, source);
        gboolean is_rename = node_id != EXPLORER_PENDING_CREATE_ID;
        FileOperationResult result = is_rename ? file_rename(root, old_relative_path, new_relative)
                                                : (is_dir ? directory_create(root, new_relative)
                                                           : file_create(root, new_relative));

        if (result != FILE_OP_OK) {
            show_explorer_error(backend, file_operation_error_message(result));
        } else if (is_rename) {
            /* Keeps any already-open editor/binary-info tab for the old
             * path pointed at the new one, so a future Save keeps
             * targeting the right file (the doc's "renamed open files
             * continue saving to their new path"). */
            editor_handle_external_rename(backend, root, old_relative_path, new_relative);
        }

        g_free(old_relative_path);
        g_free(parent_relative_path);
    } else if (trimmed[0] != '\0') {
        show_explorer_error(backend, "Names can't contain '/'.");
    }
    /* A blank name falls through untouched - treated like Escape below. */

    g_free(trimmed);
    load_row_children(backend, store, &parent_iter);
}

static void on_explorer_name_editing_canceled(GtkCellRenderer *renderer, gpointer user_data) {
    (void)renderer;
    GtkBackend *backend = user_data;
    g_object_set(backend->explorer_name_renderer, "editable", FALSE, NULL);

    if (!backend->explorer_editing_row) {
        return;
    }
    if (!gtk_tree_row_reference_valid(backend->explorer_editing_row)) {
        gtk_tree_row_reference_free(backend->explorer_editing_row);
        backend->explorer_editing_row = NULL;
        return;
    }

    GtkTreeStore *store = backend->explorer_store;
    GtkTreePath *row_path = gtk_tree_row_reference_get_path(backend->explorer_editing_row);
    GtkTreeIter iter;
    gboolean has_iter = gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter, row_path);
    gtk_tree_path_free(row_path);
    gtk_tree_row_reference_free(backend->explorer_editing_row);
    backend->explorer_editing_row = NULL;
    if (!has_iter) {
        return;
    }

    guint64 node_id = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, EXPLORER_COL_NODE_ID, &node_id, -1);
    if (node_id == EXPLORER_PENDING_CREATE_ID) {
        gtk_tree_store_remove(store, &iter);
    }
    /* A canceled rename needs no action - GTK's own cancel already
     * discarded the typed text, leaving the row's real name in place. */
}

/* Forward declared - defined in the "Explorer clipboard / drag-and-drop"
 * section below (needs explorer_permanent_root_iter/relative_path_dirname,
 * both defined further down in the "File watching" section), same
 * forward-declaration technique ui_gtk_tabs.c's own popup_tab_context_menu
 * already establishes for this exact situation. */
static void on_explorer_paste_clicked(GtkButton *button, gpointer user_data);
static void on_explorer_drag_begin(GtkWidget *tree_view, GdkDragContext *context, gpointer user_data);
static void on_explorer_drag_data_get(GtkWidget *tree_view, GdkDragContext *context, GtkSelectionData *selection_data,
                                       guint info, guint time, gpointer user_data);
static void on_explorer_drag_data_received(GtkWidget *tree_view, GdkDragContext *context, gint x, gint y,
                                            GtkSelectionData *selection_data, guint info, guint time,
                                            gpointer user_data);

/* Same-app-only target - source and dest are always this same tree view
 * in this same process, so no real payload needs to cross through
 * GtkSelectionData (see on_explorer_drag_data_get/received). */
static const GtkTargetEntry EXPLORER_DND_TARGETS[] = {
    {"application/x-toolbox-explorer-row", GTK_TARGET_SAME_APP, 0},
};

GtkWidget *build_explorer_sidebar(GtkBackend *backend) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(box, 240, -1);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *title = gtk_label_new("Explorer");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    GtkWidget *new_file_button = gtk_button_new_with_label("New File");
    GtkWidget *new_folder_button = gtk_button_new_with_label("New Folder");
    GtkWidget *refresh_button = gtk_button_new_with_label("\xE2\x86\xBB"); /* refresh: ↻ */
    GtkWidget *collapse_all_button = gtk_button_new_with_label("Collapse All");
    GtkWidget *paste_button = gtk_button_new_with_label("Paste");
    gtk_button_set_relief(GTK_BUTTON(new_file_button), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(new_folder_button), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(refresh_button), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(collapse_all_button), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(paste_button), GTK_RELIEF_NONE);
    gtk_widget_set_sensitive(paste_button, FALSE); /* enabled by explorer_set_clipboard() once Cut/Copy sets something */
    g_object_set_data(G_OBJECT(new_file_button), "toolbox-explorer-new-file-button", new_file_button);
    g_object_set_data(G_OBJECT(new_folder_button), "toolbox-explorer-new-folder-button", new_folder_button);
    g_object_set_data(G_OBJECT(refresh_button), "toolbox-explorer-refresh-button", refresh_button);
    g_object_set_data(G_OBJECT(collapse_all_button), "toolbox-explorer-collapse-all-button", collapse_all_button);
    g_object_set_data(G_OBJECT(paste_button), "toolbox-explorer-paste-button", paste_button);
    gtk_box_pack_start(GTK_BOX(toolbar), new_file_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), new_folder_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), refresh_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), collapse_all_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), paste_button, FALSE, FALSE, 0);
    backend->explorer_paste_button = paste_button;

    GtkTreeStore *store = gtk_tree_store_new(EXPLORER_COL_COUNT,
        G_TYPE_STRING,  /* icon name */
        G_TYPE_STRING,  /* display name */
        G_TYPE_STRING,  /* path */
        G_TYPE_BOOLEAN, /* is_dir */
        G_TYPE_BOOLEAN, /* loaded */
        G_TYPE_INT,     /* source */
        G_TYPE_UINT64   /* node id */
    );
    backend->explorer_store = store;

    GtkTreeIter files_root;
    gtk_tree_store_append(store, &files_root, NULL);
    gtk_tree_store_set(store, &files_root,
        EXPLORER_COL_ICON, "folder",
        EXPLORER_COL_NAME, "TOOLBOX",
        EXPLORER_COL_PATH, "",
        EXPLORER_COL_IS_DIR, TRUE,
        EXPLORER_COL_LOADED, FALSE,
        EXPLORER_COL_SOURCE, EXPLORER_SOURCE_FILES,
        EXPLORER_COL_NODE_ID, (guint64)FILE_TREE_ROOT_ID,
        -1);

    GtkTreeIter toolkit_root;
    gtk_tree_store_append(store, &toolkit_root, NULL);
    gtk_tree_store_set(store, &toolkit_root,
        EXPLORER_COL_ICON, "folder",
        EXPLORER_COL_NAME, "Toolkit",
        EXPLORER_COL_PATH, "",
        EXPLORER_COL_IS_DIR, TRUE,
        EXPLORER_COL_LOADED, FALSE,
        EXPLORER_COL_SOURCE, EXPLORER_SOURCE_TOOLKIT,
        EXPLORER_COL_NODE_ID, (guint64)0,
        -1);

    /* "Load only the root initially" - both permanent roots' own
     * immediate children are populated eagerly here; everything deeper
     * stays lazy, loaded only once its own row is expanded. */
    load_row_children(backend, store, &files_root);
    load_row_children(backend, store, &toolkit_root);

    GtkWidget *tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store); /* the tree view holds its own reference */
    g_object_set_data(G_OBJECT(tree_view), "toolbox-explorer-tree", tree_view);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree_view), FALSE);
    backend->explorer_tree_view = tree_view;

    GtkTreeViewColumn *column = gtk_tree_view_column_new();
    GtkCellRenderer *icon_renderer = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon_renderer, "stock-size", GTK_ICON_SIZE_MENU, NULL);
    gtk_tree_view_column_pack_start(column, icon_renderer, FALSE);
    gtk_tree_view_column_add_attribute(column, icon_renderer, "icon-name", EXPLORER_COL_ICON);

    GtkCellRenderer *text_renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(column, text_renderer, TRUE);
    gtk_tree_view_column_add_attribute(column, text_renderer, "text", EXPLORER_COL_NAME);
    g_object_set(text_renderer, "editable", FALSE, NULL);
    backend->explorer_name_renderer = text_renderer;

    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);
    g_signal_connect(tree_view, "row-expanded", G_CALLBACK(on_explorer_row_expanded), backend);
    g_signal_connect(tree_view, "row-activated", G_CALLBACK(on_explorer_row_activated), backend);
    g_signal_connect(tree_view, "button-press-event", G_CALLBACK(on_explorer_button_press), backend);
    g_signal_connect(tree_view, "popup-menu", G_CALLBACK(on_explorer_popup_menu), backend);
    g_signal_connect(text_renderer, "edited", G_CALLBACK(on_explorer_name_edited), backend);
    g_signal_connect(text_renderer, "editing-canceled", G_CALLBACK(on_explorer_name_editing_canceled), backend);

    /* In-app drag-and-drop (move only - Copy stays a Cut/Copy+Paste
     * action) - dragging a row onto a folder row moves it, sharing the
     * exact same perform_explorer_move() Cut+Paste uses. */
    gtk_tree_view_enable_model_drag_source(GTK_TREE_VIEW(tree_view), GDK_BUTTON1_MASK, EXPLORER_DND_TARGETS,
                                            G_N_ELEMENTS(EXPLORER_DND_TARGETS), GDK_ACTION_MOVE);
    gtk_tree_view_enable_model_drag_dest(GTK_TREE_VIEW(tree_view), EXPLORER_DND_TARGETS,
                                          G_N_ELEMENTS(EXPLORER_DND_TARGETS), GDK_ACTION_MOVE);
    g_signal_connect(tree_view, "drag-begin", G_CALLBACK(on_explorer_drag_begin), backend);
    g_signal_connect(tree_view, "drag-data-get", G_CALLBACK(on_explorer_drag_data_get), backend);
    g_signal_connect(tree_view, "drag-data-received", G_CALLBACK(on_explorer_drag_data_received), backend);

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), tree_view);

    g_signal_connect(new_file_button, "clicked", G_CALLBACK(on_explorer_new_file_clicked), backend);
    g_signal_connect(new_folder_button, "clicked", G_CALLBACK(on_explorer_new_folder_clicked), backend);
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_explorer_refresh_clicked), backend);
    g_signal_connect(collapse_all_button, "clicked", G_CALLBACK(on_explorer_collapse_all_clicked), backend);
    g_signal_connect(paste_button, "clicked", G_CALLBACK(on_explorer_paste_clicked), backend);

    gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);

    GtkTreePath *files_path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &files_root);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_view), files_path, FALSE);
    gtk_tree_path_free(files_path);

    GtkTreePath *toolkit_path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &toolkit_root);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_view), toolkit_path, FALSE);
    gtk_tree_path_free(toolkit_path);

    return box;
}
/* --- end Explorer sidebar ------------------------------------------------ */

/* --- File watching --------------------------------------------------------
 * Applies a FileWatchEvent (an external create/modify/delete/rename
 * detected by a FileWatcher, drained each tick - see ui_gtk_window.c's
 * on_tick) to both the explorer tree and any open editor tab, reusing
 * refresh_row_preserving_expansion() (Step 2) and
 * editor_handle_external_rename()/_delete()/_modification() (Steps 4/5
 * and this step) rather than any separate tree-mutation logic. */

/* Strips the final path component, leaving the parent directory's
 * root-relative path ("" if relative_path has no '/', meaning the
 * permanent root itself). */
static void relative_path_dirname(const char *relative_path, char *out, size_t out_size) {
    const char *slash = strrchr(relative_path, '/');
    if (!slash) {
        out[0] = '\0';
        return;
    }
    size_t len = (size_t)(slash - relative_path);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, relative_path, len);
    out[len] = '\0';
}

/* The merged explorer_store has exactly two top-level rows (TOOLBOX,
 * Toolkit); this finds the one matching source. */
static gboolean explorer_permanent_root_iter(GtkTreeStore *store, int source, GtkTreeIter *out) {
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter)) {
        return FALSE;
    }
    do {
        int row_source = EXPLORER_SOURCE_FILES;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, EXPLORER_COL_SOURCE, &row_source, -1);
        if (row_source == source) {
            *out = iter;
            return TRUE;
        }
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter));
    return FALSE;
}

/* Recursively searches scope (and everything already loaded beneath it)
 * for a row whose own EXPLORER_COL_PATH matches relative_path exactly -
 * every row already stores its own root-relative path directly, so this
 * needs no per-segment name matching. Only ever needs to succeed as far
 * as an already-loaded directory goes, which is guaranteed here: only a
 * loaded (and therefore watched) directory can ever produce an event for
 * a child in the first place. An unloaded row's lazy placeholder child
 * (blank name, no path) simply fails to match and terminates that
 * branch of the search. */
gboolean find_dir_iter_by_relative_path(GtkTreeModel *model, GtkTreeIter *scope, const char *relative_path,
                                         GtkTreeIter *out) {
    if (relative_path[0] == '\0') {
        *out = *scope;
        return TRUE;
    }

    gchar *scope_path = NULL;
    gtk_tree_model_get(model, scope, EXPLORER_COL_PATH, &scope_path, -1);
    gboolean is_match = scope_path && strcmp(scope_path, relative_path) == 0;
    g_free(scope_path);
    if (is_match) {
        *out = *scope;
        return TRUE;
    }

    GtkTreeIter child;
    if (!gtk_tree_model_iter_children(model, &child, scope)) {
        return FALSE;
    }
    do {
        if (find_dir_iter_by_relative_path(model, &child, relative_path, out)) {
            return TRUE;
        }
    } while (gtk_tree_model_iter_next(model, &child));
    return FALSE;
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

/* --- Explorer clipboard / drag-and-drop ------------------------------------
 * Cut/Copy/Paste (menus.c builds the menu items and the toolbar Paste
 * button lives in build_explorer_sidebar above) and in-app drag-and-drop
 * (move only). Both ultimately funnel through perform_explorer_move()/
 * perform_explorer_copy() below, so drag-and-drop's own correctness is
 * covered by construction through Cut+Paste's exhaustive test coverage -
 * see the plan file's own note on why a real X11 drag gesture isn't
 * automated here. */

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
 * refresh_parent_of above), if it's currently loaded. */
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

static void on_explorer_paste_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkBackend *backend = user_data;
    GtkTreeIter parent_iter;
    explorer_target_parent_from_selection(backend, &parent_iter);
    perform_explorer_paste(backend, &parent_iter);
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
/* --- end Explorer clipboard / drag-and-drop --------------------------------- */
