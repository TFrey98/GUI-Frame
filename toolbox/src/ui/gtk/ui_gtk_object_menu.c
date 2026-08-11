#include "ui_gtk_backend.h"
#include "ui_gtk_terminal_internal.h"

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
