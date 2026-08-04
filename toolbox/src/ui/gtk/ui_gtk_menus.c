#include "ui_gtk_internal.h"

#include <string.h>

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
    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(GTK_TREE_MODEL(store), iter, EXPLORER_COL_PATH, &relative_path, EXPLORER_COL_IS_DIR, &is_dir,
                        EXPLORER_COL_SOURCE, &source, -1);

    const WorkspaceRoot *root = explorer_root_for_source(backend, source);
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
    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &ctx->iter, EXPLORER_COL_PATH, &relative_path, EXPLORER_COL_IS_DIR,
                        &is_dir, EXPLORER_COL_NODE_ID, &node_id, EXPLORER_COL_SOURCE, &source, -1);

    const WorkspaceRoot *root = explorer_root_for_source(backend, source);
    gboolean needs_confirm;
    if (is_dir) {
        needs_confirm = !file_operations_directory_is_empty(root, relative_path);
    } else if (source == EXPLORER_SOURCE_FILES) {
        const FileTreeNode *node = file_tree_find(backend->file_tree, (FileNodeId)node_id);
        needs_confirm = node && node->executable;
    } else {
        char resolved[4096];
        bool executable = false, read_only = true;
        if (relative_path && workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
            explorer_toolkit_file_flags(resolved, &executable, &read_only);
        }
        needs_confirm = executable;
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

/* --- Step 5: terminal actions / clipboard ------------------------------ */

/* Truncates relative_path at its last '/' into out - "" (the workspace
 * root itself) if there isn't one, matching open_terminal_at()'s own
 * empty-string-means-root convention. */
static void relative_parent_dir(const char *relative_path, char *out, size_t out_size) {
    out[0] = '\0';
    if (!relative_path) {
        return;
    }
    const char *slash = strrchr(relative_path, '/');
    if (!slash) {
        return;
    }
    size_t len = (size_t)(slash - relative_path);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, relative_path, len);
    out[len] = '\0';
}

static void on_explorer_menu_open_terminal(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    gchar *relative_path = NULL;
    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->backend->explorer_store), &ctx->iter, EXPLORER_COL_PATH, &relative_path,
                        EXPLORER_COL_SOURCE, &source, -1);
    open_terminal_at(ctx->backend, explorer_root_for_source(ctx->backend, source), relative_path ? relative_path : "");
    g_free(relative_path);
}

static void on_explorer_menu_open_terminal_directory(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    gchar *relative_path = NULL;
    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->backend->explorer_store), &ctx->iter, EXPLORER_COL_PATH, &relative_path,
                        EXPLORER_COL_SOURCE, &source, -1);
    char parent[4096];
    relative_parent_dir(relative_path, parent, sizeof(parent));
    open_terminal_at(ctx->backend, explorer_root_for_source(ctx->backend, source), parent);
    g_free(relative_path);
}

/* Identical to what double-click already does (open_or_focus_file_tab) -
 * an explicit, discoverable equivalent for a script/executable row,
 * never execution itself ("do not treat a double-click as execution"
 * stays true either way). Only offered when file_classify() agrees the
 * file is actually text - see popup_explorer_context_menu. */
static void on_explorer_menu_open_as_text(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    GtkBackend *backend = ctx->backend;
    guint64 node_id = 0;
    gchar *relative_path = NULL;
    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(GTK_TREE_MODEL(backend->explorer_store), &ctx->iter, EXPLORER_COL_NODE_ID, &node_id,
                        EXPLORER_COL_PATH, &relative_path, EXPLORER_COL_SOURCE, &source, -1);
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

/* No arguments, no reuse option (that's what "Run with Arguments..."
 * is for) - always a fresh terminal, working directory defaults to the
 * file's own parent. */
static void on_explorer_menu_run_in_terminal(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    GtkBackend *backend = ctx->backend;
    gchar *relative_path = NULL;
    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(GTK_TREE_MODEL(backend->explorer_store), &ctx->iter, EXPLORER_COL_PATH, &relative_path,
                        EXPLORER_COL_SOURCE, &source, -1);

    const WorkspaceRoot *root = explorer_root_for_source(backend, source);
    char resolved[4096];
    if (!relative_path || !workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
        show_explorer_error(backend, "Can't run - the target is missing or outside the workspace.");
        g_free(relative_path);
        return;
    }

    TerminalLaunchRequest request = {0};
    g_strlcpy(request.executable, resolved, sizeof(request.executable));
    char parent[4096];
    relative_parent_dir(relative_path, parent, sizeof(parent));
    if (parent[0] != '\0') {
        workspace_root_resolve_path(root, parent, request.working_directory, sizeof(request.working_directory));
    } else {
        g_strlcpy(request.working_directory, root->canonical_path, sizeof(request.working_directory));
    }
    request.create_new_terminal = TRUE;

    run_command_in_new_terminal(backend, &request, NULL, 0);
    g_free(relative_path);
}

static void on_explorer_menu_run_with_arguments(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    GtkBackend *backend = ctx->backend;
    gchar *relative_path = NULL;
    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(GTK_TREE_MODEL(backend->explorer_store), &ctx->iter, EXPLORER_COL_PATH, &relative_path,
                        EXPLORER_COL_SOURCE, &source, -1);
    GtkWindow *parent_window = gtk_application_get_active_window(backend->gtk_app);
    open_run_with_arguments_dialog(backend, explorer_root_for_source(backend, source),
                                    relative_path ? relative_path : "", parent_window);
    g_free(relative_path);
}

static void on_explorer_menu_copy_path(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    GtkBackend *backend = ctx->backend;
    gchar *relative_path = NULL;
    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(GTK_TREE_MODEL(backend->explorer_store), &ctx->iter, EXPLORER_COL_PATH, &relative_path,
                        EXPLORER_COL_SOURCE, &source, -1);

    const WorkspaceRoot *root = explorer_root_for_source(backend, source);
    char resolved[4096];
    const char *text = root->canonical_path;
    if (relative_path && relative_path[0] != '\0' &&
        workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
        text = resolved;
    }
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
    g_free(relative_path);
}

static void on_explorer_menu_copy_relative_path(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    gchar *relative_path = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->backend->explorer_store), &ctx->iter, EXPLORER_COL_PATH, &relative_path,
                        -1);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), relative_path ? relative_path : "", -1);
    g_free(relative_path);
}

/* --- Step 7: Cut/Copy/Paste --------------------------------------------- */

static void on_explorer_menu_cut(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    gchar *relative_path = NULL;
    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->backend->explorer_store), &ctx->iter, EXPLORER_COL_PATH, &relative_path,
                        EXPLORER_COL_SOURCE, &source, -1);
    explorer_set_clipboard(ctx->backend, EXPLORER_CLIPBOARD_CUT, source, relative_path ? relative_path : "");
    g_free(relative_path);
}

static void on_explorer_menu_copy(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    gchar *relative_path = NULL;
    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->backend->explorer_store), &ctx->iter, EXPLORER_COL_PATH, &relative_path,
                        EXPLORER_COL_SOURCE, &source, -1);
    explorer_set_clipboard(ctx->backend, EXPLORER_CLIPBOARD_COPY, source, relative_path ? relative_path : "");
    g_free(relative_path);
}

static void on_explorer_menu_paste(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    perform_explorer_paste(ctx->backend, &ctx->iter);
}
/* --- end Step 7: Cut/Copy/Paste ------------------------------------------- */

/* Builds a menu for either source (a Toolkit row previously got none -
 * full parity with TOOLBOX, requested directly by the user). A folder
 * gets New File/New Folder/Rename/Cut/Copy/Delete/Paste/Refresh/
 * Properties; a file gets Rename/Cut/Copy/Delete/Properties; the two
 * permanent roots never get Rename/Cut/Copy/Delete ("the root toolbox
 * directory cannot be renamed or deleted") - TOOLBOX's/Toolkit's own
 * root menu ends up New File/New Folder/Paste/Refresh/Properties. Step
 * 7 adds Cut/Copy (any non-root row) and Paste (any folder, sensitive
 * only when the clipboard isn't empty) - both funnel into
 * perform_explorer_paste()/explorer_set_clipboard() in file_tree.c.
 * Step 5 adds, additively: a folder gets
 * Open in Integrated Terminal; a script/executable (node->executable
 * for FILES - already false for every directory per file_tree.c's own
 * scan - or explorer_toolkit_file_flags() for Toolkit, since
 * ToolkitEntry tracks neither bit) gets Open as Text (only when
 * file_classify() also agrees it's text)/Run in Terminal/Run with
 * Arguments...; an ordinary file gets Open in Terminal Directory;
 * every row gets Copy Path/Copy Relative Path. */
void popup_explorer_context_menu(GtkBackend *backend, GtkWidget *tree_view, GtkTreePath *path,
                                  GdkEventButton *event) {
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
        return;
    }

    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(model, &iter, EXPLORER_COL_SOURCE, &source, -1);

    gboolean is_dir = FALSE;
    gchar *relative_path = NULL;
    guint64 node_id = 0;
    gtk_tree_model_get(model, &iter, EXPLORER_COL_IS_DIR, &is_dir, EXPLORER_COL_PATH, &relative_path,
                        EXPLORER_COL_NODE_ID, &node_id, -1);
    gboolean is_root = gtk_tree_path_get_depth(path) == 1;
    const WorkspaceRoot *root = explorer_root_for_source(backend, source);

    gboolean is_executable = FALSE;
    if (source == EXPLORER_SOURCE_FILES) {
        const FileTreeNode *node = file_tree_find(backend->file_tree, (FileNodeId)node_id);
        is_executable = node && node->executable;
    } else if (relative_path) {
        char resolved_for_exec[4096];
        bool executable = false, read_only = true;
        if (workspace_root_resolve_path(root, relative_path, resolved_for_exec, sizeof(resolved_for_exec))) {
            explorer_toolkit_file_flags(resolved_for_exec, &executable, &read_only);
        }
        is_executable = executable;
    }

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
        add_explorer_menu_item(menu, "Cut", TRUE, G_CALLBACK(on_explorer_menu_cut), ctx);
        add_explorer_menu_item(menu, "Copy", TRUE, G_CALLBACK(on_explorer_menu_copy), ctx);
        add_explorer_menu_item(menu, "Delete", TRUE, G_CALLBACK(on_explorer_menu_delete), ctx);
    }
    if (is_dir) {
        add_explorer_menu_item(menu, "Paste", backend->explorer_clipboard.mode != EXPLORER_CLIPBOARD_NONE,
                                G_CALLBACK(on_explorer_menu_paste), ctx);
        add_explorer_menu_item(menu, "Refresh", TRUE, G_CALLBACK(on_explorer_menu_refresh), ctx);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    if (is_dir) {
        add_explorer_menu_item(menu, "Open in Integrated Terminal", TRUE, G_CALLBACK(on_explorer_menu_open_terminal),
                                ctx);
    } else if (is_executable) {
        gboolean can_open_as_text = FALSE;
        char resolved[4096];
        if (relative_path && workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
            FileClassification classification = file_classify(resolved, TRUE);
            can_open_as_text = classification.target == FILE_TARGET_EDITOR;
        }
        if (can_open_as_text) {
            add_explorer_menu_item(menu, "Open as Text", TRUE, G_CALLBACK(on_explorer_menu_open_as_text), ctx);
        }
        add_explorer_menu_item(menu, "Run in Terminal", TRUE, G_CALLBACK(on_explorer_menu_run_in_terminal), ctx);
        add_explorer_menu_item(menu, "Run with Arguments\xE2\x80\xA6", TRUE,
                                G_CALLBACK(on_explorer_menu_run_with_arguments), ctx);
    } else {
        add_explorer_menu_item(menu, "Open in Terminal Directory", TRUE,
                                G_CALLBACK(on_explorer_menu_open_terminal_directory), ctx);
    }
    add_explorer_menu_item(menu, "Copy Path", TRUE, G_CALLBACK(on_explorer_menu_copy_path), ctx);
    add_explorer_menu_item(menu, "Copy Relative Path", TRUE, G_CALLBACK(on_explorer_menu_copy_relative_path), ctx);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    add_explorer_menu_item(menu, "Properties", TRUE, G_CALLBACK(on_explorer_menu_properties), ctx);

    g_free(relative_path);
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
