#include "ui_gtk_internal.h"

#include <string.h>

/* --- Explorer sidebar ---------------------------------------------------
 * One shared tree merging two independent sources under permanent,
 * always-present root rows: "TOOLBOX" (FileTree, backed by the
 * WorkspaceRoot's files/ directory) and "Toolkit" (toolkit_index's
 * toolkit/ directory - pre-existing). Each root and every directory
 * beneath it is lazily expanded on demand, same behavior for both
 * sources, dispatched by EXPLORER_COL_SOURCE (see ui_gtk_internal.h for
 * the column/source enums and the pending-create sentinel). */

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

/* Same shape as add_files_tree_entry, for a Toolkit-sourced row. */
static void add_toolkit_tree_row(GtkTreeStore *store, GtkTreeIter *parent, const ToolkitEntry *entry) {
    GtkTreeIter iter;
    gtk_tree_store_append(store, &iter, parent);
    gtk_tree_store_set(store, &iter,
        EXPLORER_COL_ICON, entry->is_dir ? "folder" : "text-x-generic",
        EXPLORER_COL_NAME, entry->name,
        EXPLORER_COL_PATH, entry->path,
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
        char *dir_path = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(store), iter, EXPLORER_COL_PATH, &dir_path, -1);
        if (dir_path) {
            ToolkitEntry entries[TOOLKIT_INDEX_MAX_ENTRIES];
            int n = toolkit_scan_directory(dir_path, entries, TOOLKIT_INDEX_MAX_ENTRIES);
            for (int i = 0; i < n; i++) {
                add_toolkit_tree_row(store, iter, &entries[i]);
                free(entries[i].name);
                free(entries[i].path);
            }
        }
        g_free(dir_path);
    }

    for (int i = 0; i < old_count; i++) {
        GtkTreeIter old_child;
        if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(store), &old_child, iter, 0)) {
            gtk_tree_store_remove(store, &old_child);
        }
    }

    gtk_tree_store_set(store, iter, EXPLORER_COL_LOADED, TRUE, -1);
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

/* Double-click (or Enter) on a row toggles a directory row, same as
 * before Step 3 - Toolkit rows and FILES directories keep that exact
 * behavior unchanged. A non-directory FILES row instead opens (or
 * focuses) an editor/binary-info tab via open_or_focus_file_tab(). */
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
    gtk_tree_model_get(model, &iter, EXPLORER_COL_IS_DIR, &is_dir, EXPLORER_COL_SOURCE, &source,
                        EXPLORER_COL_NODE_ID, &node_id, -1);

    if (is_dir || source != EXPLORER_SOURCE_FILES) {
        if (gtk_tree_view_row_expanded(tree_view, path)) {
            gtk_tree_view_collapse_row(tree_view, path);
        } else {
            gtk_tree_view_expand_row(tree_view, path, FALSE);
        }
        return;
    }

    const FileTreeNode *node = file_tree_find(backend->file_tree, (FileNodeId)node_id);
    if (node) {
        open_or_focus_file_tab(backend, node);
    }
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
    gtk_tree_model_get(GTK_TREE_MODEL(store), parent_iter, EXPLORER_COL_LOADED, &loaded, -1);
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
        EXPLORER_COL_SOURCE, EXPLORER_SOURCE_FILES,
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

/* A selected FILES directory row is the target directly; a selected
 * FILES file row targets its parent; nothing selected (or a Toolkit row
 * selected, out of scope for file operations) defaults to the TOOLBOX
 * root - same convention VS Code's own toolbar New File/Folder uses. */
static void explorer_target_parent_from_selection(GtkBackend *backend, GtkTreeIter *out_parent_iter) {
    GtkTreeView *tree_view = GTK_TREE_VIEW(backend->explorer_tree_view);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(tree_view);
    GtkTreeModel *model;
    GtkTreeIter selected;
    if (gtk_tree_selection_get_selected(selection, &model, &selected)) {
        int source = EXPLORER_SOURCE_FILES;
        gtk_tree_model_get(model, &selected, EXPLORER_COL_SOURCE, &source, -1);
        if (source == EXPLORER_SOURCE_FILES) {
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
        gchar *old_relative_path = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, EXPLORER_COL_NODE_ID, &node_id, EXPLORER_COL_IS_DIR,
                            &is_dir, EXPLORER_COL_PATH, &old_relative_path, -1);
        gchar *parent_relative_path = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &parent_iter, EXPLORER_COL_PATH, &parent_relative_path, -1);

        char new_relative[4096];
        if (parent_relative_path && parent_relative_path[0] != '\0') {
            snprintf(new_relative, sizeof(new_relative), "%s/%s", parent_relative_path, trimmed);
        } else {
            snprintf(new_relative, sizeof(new_relative), "%s", trimmed);
        }

        const WorkspaceRoot *root = workbench_get_file_workspace_root(backend->workbench);
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
            editor_handle_external_rename(backend, old_relative_path, new_relative);
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
    gtk_button_set_relief(GTK_BUTTON(new_file_button), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(new_folder_button), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(refresh_button), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(collapse_all_button), GTK_RELIEF_NONE);
    g_object_set_data(G_OBJECT(new_file_button), "toolbox-explorer-new-file-button", new_file_button);
    g_object_set_data(G_OBJECT(new_folder_button), "toolbox-explorer-new-folder-button", new_folder_button);
    g_object_set_data(G_OBJECT(refresh_button), "toolbox-explorer-refresh-button", refresh_button);
    g_object_set_data(G_OBJECT(collapse_all_button), "toolbox-explorer-collapse-all-button", collapse_all_button);
    gtk_box_pack_start(GTK_BOX(toolbar), new_file_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), new_folder_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), refresh_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), collapse_all_button, FALSE, FALSE, 0);

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
        EXPLORER_COL_PATH, toolkit_index_dir(),
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

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), tree_view);

    g_signal_connect(new_file_button, "clicked", G_CALLBACK(on_explorer_new_file_clicked), backend);
    g_signal_connect(new_folder_button, "clicked", G_CALLBACK(on_explorer_new_folder_clicked), backend);
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_explorer_refresh_clicked), backend);
    g_signal_connect(collapse_all_button, "clicked", G_CALLBACK(on_explorer_collapse_all_clicked), backend);

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
