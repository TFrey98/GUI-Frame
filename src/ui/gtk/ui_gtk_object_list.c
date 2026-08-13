#include "ui_gtk_backend.h"
#include "ui_gtk_tabs_internal.h"
#include "ui_gtk_terminal_internal.h"

#include <stdio.h>

/* Shared by the listener console tab and the New Listener dialog's
 * status feedback. */
const char *listener_state_name(ListenerState state) {
    switch (state) {
        case LISTENER_STATE_CONFIGURED: return "CONFIGURED";
        case LISTENER_STATE_STARTING: return "STARTING";
        case LISTENER_STATE_RUNNING: return "RUNNING";
        case LISTENER_STATE_STOPPING: return "STOPPING";
        case LISTENER_STATE_STOPPED: return "STOPPED";
        case LISTENER_STATE_ERROR: return "ERROR";
    }
    return "?";
}

/* Shared by the listener console tab's subtitle. */
const char *listener_type_label(ListenerType type) {
    switch (type) {
        case LISTENER_TYPE_REVERSE_TCP: return "TCP";
        case LISTENER_TYPE_HTTP: return "HTTP";
        case LISTENER_TYPE_HTTPS: return "HTTPS";
    }
    return "?";
}

/* Shared by the bottom object panel. */
const char *connection_state_name(ConnectionState state) {
    switch (state) {
        case CONNECTION_STATE_CONNECTED: return "CONNECTED";
        case CONNECTION_STATE_DISCONNECTED: return "DISCONNECTED";
    }
    return "?";
}

/* --- Bottom object panel -----------------------------------------------
 * Tree from the registry: listeners -> their connections. Refreshed every
 * tick (see on_tick/refresh_object_panel) rather than on a manual action
 * like the Toolkit sidebar's refresh button, so connections appear live
 * as they connect. That live-refresh
 * requirement is why this syncs rows in place instead of the sidebar's
 * clear-and-repopulate: clearing every 100ms would collapse any row the
 * user had expanded on every single tick.
 *
 * The sync below is bidirectional: it updates/appends live objects and
 * removes rows whose registry objects were removed. See ui_gtk_backend.h
 * for OBJECT_PANEL_COL_*'s declaration. */

static void sync_listener_row(GtkTreeStore *store, GtkTreeIter *iter, const Listener *listener) {
    char endpoint[160];
    /* Always the callback endpoint, never bind_address (which may be a
     * 0.0.0.0 wildcard) - same convention as the listener console tab. */
    snprintf(endpoint, sizeof(endpoint), "%s:%u", listener->config.callback_host, listener->config.port);
    gtk_tree_store_set(store, iter,
        OBJECT_PANEL_COL_NAME, listener->config.name,
        OBJECT_PANEL_COL_ENDPOINT, endpoint,
        OBJECT_PANEL_COL_STATE, listener_state_name(listener->runtime.state),
        OBJECT_PANEL_COL_ID, (guint64)listener->id,
        -1);
}

static void sync_connection_row(GtkTreeStore *store, GtkTreeIter *iter, const Connection *connection) {
    char endpoint[96];
    snprintf(endpoint, sizeof(endpoint), "%s:%u", connection->remote_host, connection->remote_port);
    gtk_tree_store_set(store, iter,
        OBJECT_PANEL_COL_NAME, "",
        OBJECT_PANEL_COL_ENDPOINT, endpoint,
        OBJECT_PANEL_COL_STATE, connection_state_name(connection->state),
        OBJECT_PANEL_COL_ID, (guint64)connection->id,
        -1);
}

static const Connection *find_connection_by_id(const Connection **connections, int n, guint64 id) {
    for (int i = 0; i < n; i++) {
        if (connections[i]->id == id) {
            return connections[i];
        }
    }
    return NULL;
}

/* Pass 1 walks the store's existing children, updating any whose
 * connection still exists and removing any that don't (gtk_tree_store_remove() returns
 * the next iter directly, so this is still a single walk); pass 2
 * appends anything the registry has that the store doesn't yet.
 * Matching is by OBJECT_PANEL_COL_ID, not position, since removal can
 * now leave gaps a positional walk would mismatch against. */
static void sync_connections_for_listener(GtkTreeStore *store, GtkTreeIter *listener_iter, ObjectRegistry *registry,
                                           uint64_t listener_id) {
    const Connection *connections[64];
    int n = object_registry_list_connections_for_listener(registry, listener_id, connections, 64);

    GtkTreeIter child;
    gboolean has_child = gtk_tree_model_iter_children(GTK_TREE_MODEL(store), &child, listener_iter);
    while (has_child) {
        guint64 id = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &child, OBJECT_PANEL_COL_ID, &id, -1);
        const Connection *connection = find_connection_by_id(connections, n, id);
        if (connection) {
            sync_connection_row(store, &child, connection);
            has_child = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &child);
        } else {
            has_child = gtk_tree_store_remove(store, &child);
        }
    }

    for (int i = 0; i < n; i++) {
        gboolean found = FALSE;
        GtkTreeIter existing;
        if (gtk_tree_model_iter_children(GTK_TREE_MODEL(store), &existing, listener_iter)) {
            do {
                guint64 id = 0;
                gtk_tree_model_get(GTK_TREE_MODEL(store), &existing, OBJECT_PANEL_COL_ID, &id, -1);
                if (id == connections[i]->id) {
                    found = TRUE;
                    break;
                }
            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &existing));
        }
        if (!found) {
            GtkTreeIter new_child;
            gtk_tree_store_append(store, &new_child, listener_iter);
            sync_connection_row(store, &new_child, connections[i]);
        }
    }
}

/* Called every tick (see on_tick) to keep the bottom panel's tree in sync
 * with the registry without ever clearing it - see the section comment.
 * Same bidirectional two-pass shape as sync_connections_for_listener,
 * one level up. */
void refresh_object_panel(GtkBackend *backend) {
    GtkTreeStore *store = backend->object_panel_store;
    ObjectRegistry *registry = backend->listener_system->registry;
    int count = object_registry_listener_count(registry);

    GtkTreeIter iter;
    gboolean has_row = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);
    while (has_row) {
        guint64 id = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, OBJECT_PANEL_COL_ID, &id, -1);
        const Listener *listener = object_registry_get_listener(registry, id);
        if (listener) {
            sync_listener_row(store, &iter, listener);
            sync_connections_for_listener(store, &iter, registry, listener->id);
            has_row = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);
        } else {
            has_row = gtk_tree_store_remove(store, &iter);
        }
    }

    for (int i = 0; i < count; i++) {
        const Listener *listener = object_registry_get_listener_at(registry, i);
        gboolean found = FALSE;
        GtkTreeIter existing;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &existing)) {
            do {
                guint64 id = 0;
                gtk_tree_model_get(GTK_TREE_MODEL(store), &existing, OBJECT_PANEL_COL_ID, &id, -1);
                if (id == listener->id) {
                    found = TRUE;
                    break;
                }
            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &existing));
        }
        if (found) {
            continue;
        }
        GtkTreeIter new_iter;
        gtk_tree_store_append(store, &new_iter, NULL);
        sync_listener_row(store, &new_iter, listener);
        sync_connections_for_listener(store, &new_iter, registry, listener->id);
    }
}

/* workspace_open_object: dispatches a bottom-panel row double-click by
 * type/state - specifically by tree depth here, since the object panel
 * only ever has two levels (listeners at depth 1, their connections at
 * depth 2). State-driven behavior (read-only vs interactive) lives
 * inside the connection page itself, not in this dispatch - opening is
 * always allowed regardless of state (object_can_open_terminal is
 * unconditionally true for a Connection). */
static void on_object_panel_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column,
                                           gpointer user_data) {
    (void)column;
    GtkBackend *backend = user_data;
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
        return;
    }
    guint64 id = 0;
    gtk_tree_model_get(model, &iter, OBJECT_PANEL_COL_ID, &id, -1);

    int depth = gtk_tree_path_get_depth(path);
    if (depth == 1) {
        focus_or_open_listener_tab(backend, id);
    } else if (depth == 2) {
        open_or_focus_connection_terminal(backend, id);
    }
}

GtkWidget *build_bottom_panel(GtkBackend *backend) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(box, -1, 160);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);

    backend->object_panel_store = gtk_tree_store_new(OBJECT_PANEL_COL_COUNT,
        G_TYPE_STRING,  /* name */
        G_TYPE_STRING,  /* endpoint */
        G_TYPE_STRING,  /* state */
        G_TYPE_UINT64   /* object id */
    );

    GtkWidget *tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(backend->object_panel_store));
    g_object_unref(backend->object_panel_store); /* the tree view holds its own reference */
    g_object_set_data(G_OBJECT(tree_view), "workbench-object-panel-tree", tree_view);
    g_signal_connect(tree_view, "row-activated", G_CALLBACK(on_object_panel_row_activated), backend);
    g_signal_connect(tree_view, "button-press-event", G_CALLBACK(on_object_panel_button_press), backend);
    g_signal_connect(tree_view, "popup-menu", G_CALLBACK(on_object_panel_popup_menu), backend);

    static const struct {
        const char *title;
        int column;
    } columns[] = {
        {"Name", OBJECT_PANEL_COL_NAME},
        {"Endpoint", OBJECT_PANEL_COL_ENDPOINT},
        {"State", OBJECT_PANEL_COL_STATE},
    };
    for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *column =
            gtk_tree_view_column_new_with_attributes(columns[i].title, renderer, "text", columns[i].column, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);
    }

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), tree_view);

    gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);
    return box;
}
/* --- end Bottom object panel --------------------------------------------- */
