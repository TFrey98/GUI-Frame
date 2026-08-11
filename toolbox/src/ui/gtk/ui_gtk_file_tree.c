#include "ui_gtk_backend.h"
#include "ui_gtk_explorer_internal.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- Explorer tree core --------------------------------------------------
 * Tree-store population, lazy loading, and refresh for the merged
 * explorer sidebar - two independent sources under permanent, always-
 * present root rows: "TOOLBOX" (FileTree, backed by the WorkspaceRoot's
 * files/ directory) and "Toolkit" (toolkit_index's toolkit/ directory).
 * Widget construction/inline editing lives in ui_gtk_explorer_sidebar.c;
 * watch registration/application in ui_gtk_file_watch.c; Cut/Copy/Paste/
 * drag-and-drop in ui_gtk_explorer_transfer.c; Reveal in Explorer in
 * ui_gtk_explorer_navigation.c - all built on the primitives here. */

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
 * re-scanning. Promoted (non-static) - wired as the "row-expanded"
 * signal handler in ui_gtk_explorer_sidebar.c's build_explorer_sidebar. */
void on_explorer_row_expanded(GtkTreeView *tree_view, GtkTreeIter *iter, GtkTreePath *tree_path, gpointer user_data) {
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

/* Promoted (non-static) - also used by ui_gtk_explorer_navigation.c's
 * reveal_in_explorer() to walk down by name segment. */
gboolean explorer_find_child_by_name(GtkTreeModel *model, GtkTreeIter *parent, const char *name, GtkTreeIter *out) {
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

/* Refreshes iter's own children, then re-expands and recursively refreshes
 * any new child whose
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

/* Strips the final path component, leaving the parent directory's
 * root-relative path ("" if relative_path has no '/', meaning the
 * permanent root itself). Promoted (non-static) - used by
 * ui_gtk_file_watch.c's refresh_parent_of() and
 * ui_gtk_explorer_transfer.c's perform_explorer_move(). */
void relative_path_dirname(const char *relative_path, char *out, size_t out_size) {
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
 * Toolkit); this finds the one matching source. Promoted (non-static) -
 * used by ui_gtk_file_watch.c, ui_gtk_explorer_transfer.c, and
 * ui_gtk_explorer_navigation.c. */
gboolean explorer_permanent_root_iter(GtkTreeStore *store, int source, GtkTreeIter *out) {
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
/* --- end Explorer tree core ------------------------------------------------ */
