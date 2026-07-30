#include "ui_gtk_internal.h"

/* --- Object context menu -------------------------------------------------
 * Right-click (or Shift+F10/Menu key) on a bottom-panel row. One uniform
 * set of items built the same way for both object types every time -
 * Start/Stop/Restart/Open Terminal/Wait for Reconnection gated by their
 * matching object_can_* predicate (Phase 1), Remove always sensitive
 * since there isn't a predicate for it, just a confirmation guard when
 * the target is currently running/connected. */

typedef struct MenuItemContext {
    GtkBackend *backend;
    uint64_t id;
    int depth; /* 1 = listener, 2 = connection - matches the object panel's tree depth */
} MenuItemContext;

static void add_object_menu_item(GtkWidget *menu, const char *label, gboolean sensitive, GCallback callback,
                                  MenuItemContext *ctx) {
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    gtk_widget_set_sensitive(item, sensitive);
    g_signal_connect(item, "activate", callback, ctx);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static void on_menu_start(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    MenuItemContext *ctx = user_data;
    if (ctx->depth == 1) {
        listener_manager_start_async(ctx->backend->listener_system->listener_manager, ctx->id);
    }
}

static void on_menu_stop(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    MenuItemContext *ctx = user_data;
    if (ctx->depth == 1) {
        listener_manager_stop(ctx->backend->listener_system->listener_manager, ctx->id);
    } else {
        connection_manager_disconnect(ctx->backend->listener_system->connection_manager, ctx->id);
    }
}

static void on_menu_restart(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    MenuItemContext *ctx = user_data;
    if (ctx->depth == 1) {
        listener_manager_restart(ctx->backend->listener_system->listener_manager, ctx->id);
    }
}

/* Open Terminal and Wait for Reconnection both just open/focus the
 * connection's own view. The doc's invariants distinguish "reopen" (no
 * network op) from "wait for reconnection" (a future new inbound
 * socket), but no phase so far builds the machinery to correlate a new
 * Connection back to a disconnected one by remote address - a
 * deliberate scope trim, not an oversight. */
static void on_menu_open_terminal(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    MenuItemContext *ctx = user_data;
    if (ctx->depth == 2) {
        open_or_focus_connection_terminal(ctx->backend, ctx->id);
    }
}

static void on_menu_wait_for_reconnection(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    MenuItemContext *ctx = user_data;
    if (ctx->depth == 2) {
        open_or_focus_connection_terminal(ctx->backend, ctx->id);
    }
}

static void perform_remove(GtkBackend *backend, int depth, uint64_t id) {
    if (depth == 1) {
        listener_manager_remove(backend->listener_system->listener_manager, id);
    } else {
        connection_manager_remove(backend->listener_system->connection_manager, id);
    }
}

typedef struct RemoveConfirmContext {
    GtkBackend *backend;
    int depth;
    uint64_t id;
} RemoveConfirmContext;

static void on_remove_confirm_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    RemoveConfirmContext *ctx = user_data;
    if (response_id == GTK_RESPONSE_YES) {
        perform_remove(ctx->backend, ctx->depth, ctx->id);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
    g_free(ctx);
}

/* Re-checks the object's *current* state rather than trusting whatever
 * popup_object_context_menu saw when the menu was built - time may have
 * passed between opening the menu and clicking Remove. Not running/
 * connected: removes immediately. Running/connected: a real (non-
 * blocking, "response"-signal, same convention as the New Listener
 * dialog) confirmation - confirming calls listener_manager_remove()/
 * connection_manager_remove(), which is itself already the guarded
 * stop-then-remove path, so there's no separate "force remove without
 * stopping" option. */
static void on_menu_remove(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    MenuItemContext *ctx = user_data;
    GtkBackend *backend = ctx->backend;
    ObjectRegistry *registry = backend->listener_system->registry;

    gboolean running = FALSE;
    if (ctx->depth == 1) {
        const Listener *listener = object_registry_get_listener(registry, ctx->id);
        running = listener && listener->runtime.state == LISTENER_STATE_RUNNING;
    } else {
        const Connection *connection = object_registry_get_connection(registry, ctx->id);
        running = connection && connection->state == CONNECTION_STATE_CONNECTED;
    }

    if (!running) {
        perform_remove(backend, ctx->depth, ctx->id);
        return;
    }

    RemoveConfirmContext *confirm_ctx = g_new(RemoveConfirmContext, 1);
    confirm_ctx->backend = backend;
    confirm_ctx->depth = ctx->depth;
    confirm_ctx->id = ctx->id;

    GtkWindow *parent = gtk_application_get_active_window(backend->gtk_app);
    GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        ctx->depth == 1 ? "This listener is running. Remove it anyway?"
                         : "This connection is active. Remove it anyway?");
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Remove", GTK_RESPONSE_YES);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    g_signal_connect(dialog, "response", G_CALLBACK(on_remove_confirm_response), confirm_ctx);
    gtk_widget_show_all(dialog);
}

/* Resolves path's ManagedObject, builds the six-item menu, and pops it
 * up - at event's pointer position for a mouse right-click, or centered
 * on tree_view for the keyboard "popup-menu" signal (event NULL there).
 * Tags the built menu on tree_view itself so it stays discoverable
 * after this call returns - tests read the menu this way; a real user
 * never needs to. */
void popup_object_context_menu(GtkBackend *backend, GtkWidget *tree_view, GtkTreePath *path, GdkEventButton *event) {
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
        return;
    }
    guint64 id = 0;
    gtk_tree_model_get(model, &iter, OBJECT_PANEL_COL_ID, &id, -1);
    int depth = gtk_tree_path_get_depth(path);

    ManagedObject obj;
    if (depth == 1) {
        const Listener *listener = object_registry_get_listener(backend->listener_system->registry, id);
        if (!listener) {
            return;
        }
        obj.type = MANAGED_OBJECT_LISTENER;
        obj.listener = *listener;
    } else if (depth == 2) {
        const Connection *connection = object_registry_get_connection(backend->listener_system->registry, id);
        if (!connection) {
            return;
        }
        obj.type = MANAGED_OBJECT_CONNECTION;
        obj.connection = *connection;
    } else {
        return;
    }

    MenuItemContext *ctx = g_new(MenuItemContext, 1);
    ctx->backend = backend;
    ctx->id = id;
    ctx->depth = depth;

    GtkWidget *menu = gtk_menu_new();
    g_object_set_data_full(G_OBJECT(menu), "toolbox-menu-context", ctx, g_free);
    g_object_set_data(G_OBJECT(tree_view), "toolbox-object-context-menu", menu);

    add_object_menu_item(menu, "Start", object_can_start(&obj), G_CALLBACK(on_menu_start), ctx);
    add_object_menu_item(menu, "Stop", object_can_stop(&obj), G_CALLBACK(on_menu_stop), ctx);
    add_object_menu_item(menu, "Restart", object_can_restart(&obj), G_CALLBACK(on_menu_restart), ctx);
    add_object_menu_item(menu, "Open Terminal", object_can_open_terminal(&obj), G_CALLBACK(on_menu_open_terminal),
                          ctx);
    add_object_menu_item(menu, "Wait for Reconnection", object_can_wait_for_reconnection(&obj),
                          G_CALLBACK(on_menu_wait_for_reconnection), ctx);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    add_object_menu_item(menu, "Remove", TRUE, G_CALLBACK(on_menu_remove), ctx);

    gtk_widget_show_all(menu);
    if (event) {
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    } else {
        gtk_menu_popup_at_widget(GTK_MENU(menu), tree_view, GDK_GRAVITY_CENTER, GDK_GRAVITY_CENTER, NULL);
    }
}

gboolean on_object_panel_button_press(GtkWidget *tree_view, GdkEventButton *event, gpointer user_data) {
    if (event->type != GDK_BUTTON_PRESS || event->button != 3) {
        return FALSE;
    }
    GtkBackend *backend = user_data;
    GtkTreePath *path = NULL;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree_view), (gint)event->x, (gint)event->y, &path, NULL, NULL,
                                       NULL)) {
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view)), path);
        popup_object_context_menu(backend, tree_view, path, event);
        gtk_tree_path_free(path);
    }
    return TRUE;
}

/* GTK's own keyboard-triggered-context-menu signal (Shift+F10/Menu key)
 * - acts on whatever row is currently selected. Also the test's entry
 * point: g_signal_emit_by_name(tree_view, "popup-menu", &handled) is a
 * real, standard GTK signal emission (unlike simulating VTE's "commit",
 * which Phase 9 deliberately avoided since that would fake raw user
 * input) - select a row, emit this, inspect the resulting menu. */
gboolean on_object_panel_popup_menu(GtkWidget *tree_view, gpointer user_data) {
    GtkBackend *backend = user_data;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        return FALSE;
    }
    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
    popup_object_context_menu(backend, tree_view, path, NULL);
    gtk_tree_path_free(path);
    return TRUE;
}
/* --- end Object context menu ----------------------------------------- */

/* --- Explorer context menu ------------------------------------------------
 * Right-click (or Shift+F10/Menu key) on an explorer row. See
 * ui_gtk_internal.h for ExplorerMenuContext's own comment on why it's
 * shared (dialogs.c's Properties handler also reads it). */

static void add_explorer_menu_item(GtkWidget *menu, const char *label, gboolean sensitive, GCallback callback,
                                    ExplorerMenuContext *ctx) {
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    gtk_widget_set_sensitive(item, sensitive);
    g_signal_connect(item, "activate", callback, ctx);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static void on_explorer_menu_new_file(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    start_new_entry(ctx->backend, &ctx->iter, FALSE);
}

static void on_explorer_menu_new_folder(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    start_new_entry(ctx->backend, &ctx->iter, TRUE);
}

static void on_explorer_menu_rename(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    GtkBackend *backend = ctx->backend;
    GtkTreeStore *store = backend->explorer_store;
    GtkTreeView *tree_view = GTK_TREE_VIEW(backend->explorer_tree_view);

    GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &ctx->iter);
    if (backend->explorer_editing_row) {
        gtk_tree_row_reference_free(backend->explorer_editing_row);
    }
    backend->explorer_editing_row = gtk_tree_row_reference_new(GTK_TREE_MODEL(store), path);
    g_object_set(backend->explorer_name_renderer, "editable", TRUE, NULL);
    gtk_tree_view_set_cursor_on_cell(tree_view, path, gtk_tree_view_get_column(tree_view, 0),
                                      backend->explorer_name_renderer, TRUE);
    gtk_tree_path_free(path);
}

static void on_explorer_menu_refresh(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    refresh_row_preserving_expansion(ctx->backend, GTK_TREE_VIEW(ctx->backend->explorer_tree_view),
                                      ctx->backend->explorer_store, &ctx->iter);
}

/* The actual delete - shared by the immediate and confirmed paths.
 * Directories always delete recursively: confirming the non-empty-
 * folder prompt already means "yes, delete everything in it," and an
 * empty folder's recursive walk is trivially just itself. */
static void perform_explorer_delete(GtkBackend *backend, GtkTreeIter *iter, GtkTreeIter *parent_iter) {
    GtkTreeStore *store = backend->explorer_store;
    gchar *relative_path = NULL;
    gboolean is_dir = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(store), iter, EXPLORER_COL_PATH, &relative_path, EXPLORER_COL_IS_DIR, &is_dir,
                        -1);

    const WorkspaceRoot *root = workbench_get_file_workspace_root(backend->workbench);
    FileOperationResult result = file_delete(root, relative_path, is_dir);
    if (result != FILE_OP_OK) {
        show_explorer_error(backend, file_operation_error_message(result));
    }
    g_free(relative_path);

    load_row_children(backend, store, parent_iter);
}

typedef struct ExplorerDeleteContext {
    GtkBackend *backend;
    GtkTreeRowReference *row;
    GtkTreeRowReference *parent_row;
} ExplorerDeleteContext;

static void on_explorer_delete_confirm_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    ExplorerDeleteContext *ctx = user_data;
    if (response_id == GTK_RESPONSE_YES && gtk_tree_row_reference_valid(ctx->row) &&
        gtk_tree_row_reference_valid(ctx->parent_row)) {
        GtkTreeStore *store = ctx->backend->explorer_store;
        GtkTreePath *row_path = gtk_tree_row_reference_get_path(ctx->row);
        GtkTreePath *parent_path = gtk_tree_row_reference_get_path(ctx->parent_row);
        GtkTreeIter iter, parent_iter;
        gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter, row_path);
        gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &parent_iter, parent_path);
        gtk_tree_path_free(row_path);
        gtk_tree_path_free(parent_path);
        perform_explorer_delete(ctx->backend, &iter, &parent_iter);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
    gtk_tree_row_reference_free(ctx->row);
    gtk_tree_row_reference_free(ctx->parent_row);
    g_free(ctx);
}

/* Confirms first for a non-empty folder or an executable file -
 * anything else (an empty folder, an ordinary closed file) deletes
 * immediately. Mirrors on_menu_remove's exact three-part shape. */
static void on_explorer_menu_delete(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    GtkBackend *backend = ctx->backend;
    GtkTreeStore *store = backend->explorer_store;

    GtkTreeIter parent_iter;
    if (!gtk_tree_model_iter_parent(GTK_TREE_MODEL(store), &parent_iter, &ctx->iter)) {
        return; /* a permanent root - never offered Delete, stay defensive */
    }

    gchar *relative_path = NULL;
    gboolean is_dir = FALSE;
    guint64 node_id = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &ctx->iter, EXPLORER_COL_PATH, &relative_path, EXPLORER_COL_IS_DIR,
                        &is_dir, EXPLORER_COL_NODE_ID, &node_id, -1);

    const WorkspaceRoot *root = workbench_get_file_workspace_root(backend->workbench);
    gboolean needs_confirm;
    if (is_dir) {
        needs_confirm = !file_operations_directory_is_empty(root, relative_path);
    } else {
        const FileTreeNode *node = file_tree_find(backend->file_tree, (FileNodeId)node_id);
        needs_confirm = node && node->executable;
    }
    g_free(relative_path);

    if (!needs_confirm) {
        perform_explorer_delete(backend, &ctx->iter, &parent_iter);
        return;
    }

    GtkTreePath *row_path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &ctx->iter);
    GtkTreePath *parent_path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &parent_iter);
    ExplorerDeleteContext *dctx = g_new(ExplorerDeleteContext, 1);
    dctx->backend = backend;
    dctx->row = gtk_tree_row_reference_new(GTK_TREE_MODEL(store), row_path);
    dctx->parent_row = gtk_tree_row_reference_new(GTK_TREE_MODEL(store), parent_path);
    gtk_tree_path_free(row_path);
    gtk_tree_path_free(parent_path);

    GtkWindow *parent_window = gtk_application_get_active_window(backend->gtk_app);
    GtkWidget *dialog = gtk_message_dialog_new(parent_window, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                                GTK_BUTTONS_NONE,
                                                is_dir ? "This folder isn't empty. Delete it and everything in it?"
                                                       : "This file is executable. Delete it anyway?");
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Delete", GTK_RESPONSE_YES);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    g_signal_connect(dialog, "response", G_CALLBACK(on_explorer_delete_confirm_response), dctx);
    gtk_widget_show_all(dialog);
}

/* Builds a menu only for FILES-sourced rows - a Toolkit row gets none,
 * unchanged from before this step. A folder gets New File/New Folder/
 * Rename/Delete/Refresh/Properties; a file gets Rename/Delete/
 * Properties; the two permanent roots never get Rename/Delete ("the
 * root toolbox directory cannot be renamed or deleted") - TOOLBOX's own
 * menu ends up New File/New Folder/Refresh/Properties. */
void popup_explorer_context_menu(GtkBackend *backend, GtkWidget *tree_view, GtkTreePath *path,
                                  GdkEventButton *event) {
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
        return;
    }

    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(model, &iter, EXPLORER_COL_SOURCE, &source, -1);
    if (source != EXPLORER_SOURCE_FILES) {
        return;
    }

    gboolean is_dir = FALSE;
    gtk_tree_model_get(model, &iter, EXPLORER_COL_IS_DIR, &is_dir, -1);
    gboolean is_root = gtk_tree_path_get_depth(path) == 1;

    ExplorerMenuContext *ctx = g_new(ExplorerMenuContext, 1);
    ctx->backend = backend;
    ctx->iter = iter;

    GtkWidget *menu = gtk_menu_new();
    g_object_set_data_full(G_OBJECT(menu), "toolbox-explorer-menu-context", ctx, g_free);
    g_object_set_data(G_OBJECT(tree_view), "toolbox-explorer-context-menu", menu);

    if (is_dir) {
        add_explorer_menu_item(menu, "New File", TRUE, G_CALLBACK(on_explorer_menu_new_file), ctx);
        add_explorer_menu_item(menu, "New Folder", TRUE, G_CALLBACK(on_explorer_menu_new_folder), ctx);
    }
    if (!is_root) {
        add_explorer_menu_item(menu, "Rename", TRUE, G_CALLBACK(on_explorer_menu_rename), ctx);
        add_explorer_menu_item(menu, "Delete", TRUE, G_CALLBACK(on_explorer_menu_delete), ctx);
    }
    if (is_dir) {
        add_explorer_menu_item(menu, "Refresh", TRUE, G_CALLBACK(on_explorer_menu_refresh), ctx);
    }
    add_explorer_menu_item(menu, "Properties", TRUE, G_CALLBACK(on_explorer_menu_properties), ctx);

    gtk_widget_show_all(menu);
    if (event) {
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    } else {
        gtk_menu_popup_at_widget(GTK_MENU(menu), tree_view, GDK_GRAVITY_CENTER, GDK_GRAVITY_CENTER, NULL);
    }
}

gboolean on_explorer_button_press(GtkWidget *tree_view, GdkEventButton *event, gpointer user_data) {
    if (event->type != GDK_BUTTON_PRESS || event->button != 3) {
        return FALSE;
    }
    GtkBackend *backend = user_data;
    GtkTreePath *path = NULL;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree_view), (gint)event->x, (gint)event->y, &path, NULL, NULL,
                                       NULL)) {
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view)), path);
        popup_explorer_context_menu(backend, tree_view, path, event);
        gtk_tree_path_free(path);
    }
    return TRUE;
}

gboolean on_explorer_popup_menu(GtkWidget *tree_view, gpointer user_data) {
    GtkBackend *backend = user_data;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        return FALSE;
    }
    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
    popup_explorer_context_menu(backend, tree_view, path, NULL);
    gtk_tree_path_free(path);
    return TRUE;
}
/* --- end Explorer context menu -------------------------------------------- */
