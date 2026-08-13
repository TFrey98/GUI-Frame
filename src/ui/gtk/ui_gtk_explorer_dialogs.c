#include "ui_gtk_backend.h"
#include "ui_gtk_explorer_internal.h"

/* --- Properties dialog --------------------------------------------------
 * Triggered from the explorer's context menu, but its whole effect is
 * unconditionally showing a dialog - lives here rather than
 * ui_gtk_explorer_menu.c to keep menu construction separate from dialogs.
 * Only its G_CALLBACK reference lives in the menu's own item-building code. */

static void on_explorer_properties_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    (void)response_id;
    (void)user_data;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

void on_explorer_menu_properties(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    GtkBackend *backend = ctx->backend;
    GtkTreeStore *store = backend->explorer_store;

    gchar *name = NULL;
    gchar *relative_path = NULL;
    gboolean is_dir = FALSE;
    guint64 node_id = 0;
    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &ctx->iter, EXPLORER_COL_NAME, &name, EXPLORER_COL_PATH,
                        &relative_path, EXPLORER_COL_IS_DIR, &is_dir, EXPLORER_COL_NODE_ID, &node_id,
                        EXPLORER_COL_SOURCE, &source, -1);

    gboolean read_only, executable;
    if (source == EXPLORER_SOURCE_FILES) {
        const FileTreeNode *node = file_tree_find(backend->file_tree, (FileNodeId)node_id);
        read_only = node && node->read_only;
        executable = node && node->executable;
    } else {
        bool stat_executable = false, stat_read_only = true;
        const WorkspaceRoot *root = explorer_root_for_source(backend, source);
        char resolved[4096];
        if (relative_path && relative_path[0] != '\0' &&
            workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
            explorer_toolkit_file_flags(resolved, &stat_executable, &stat_read_only);
        }
        read_only = stat_read_only;
        executable = stat_executable;
    }

    GtkWindow *parent = gtk_application_get_active_window(backend->gtk_app);
    GtkWidget *dialog =
        gtk_dialog_new_with_buttons("Properties", parent, GTK_DIALOG_MODAL, "_OK", GTK_RESPONSE_OK, NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_container_add(GTK_CONTAINER(content), grid);

    int row = 0;
    add_form_row(GTK_GRID(grid), row++, "Name", gtk_label_new(name ? name : ""));
    add_form_row(GTK_GRID(grid), row++, "Type", gtk_label_new(is_dir ? "Folder" : "File"));
    add_form_row(GTK_GRID(grid), row++, "Path",
                 gtk_label_new((relative_path && relative_path[0]) ? relative_path : "/"));
    add_form_row(GTK_GRID(grid), row++, "Read-only", gtk_label_new(read_only ? "Yes" : "No"));
    add_form_row(GTK_GRID(grid), row++, "Executable", gtk_label_new(executable ? "Yes" : "No"));

    g_signal_connect(dialog, "response", G_CALLBACK(on_explorer_properties_response), NULL);
    gtk_widget_show_all(dialog);

    g_free(name);
    g_free(relative_path);
}
/* --- end Properties dialog ------------------------------------------- */
