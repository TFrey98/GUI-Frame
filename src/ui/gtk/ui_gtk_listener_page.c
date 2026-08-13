#include "ui_gtk_backend.h"
#include "ui_gtk_tabs_internal.h"

/* --- Listener console tab --------------------------------------------
 * Opened automatically (see on_tick) once a listener's worker actually
 * confirms it's bound (LISTENER_EVENT_STARTED) - never at creation
 * time, so a listener that fails to bind never gets a dead tab.
 * Closing it is handled entirely by ui_gtk_tab_labels.c's
 * on_tab_close_clicked (via ui_gtk_tab_close.c's confirmation flow) -
 * just removes the tab, never touches the listener itself. */

/* tab->backend_data holds the listener_id directly (cast through
 * uintptr_t - safe on this project's only target, 64-bit Linux, where
 * a pointer and a uint64_t are the same size), not an owned pointer
 * like TerminalSession* - there's no separate object for this tab to
 * own, ObjectRegistry already owns the real Listener. */
static uint64_t listener_tab_id(const Tab *tab) {
    return (uint64_t)(uintptr_t)tab->backend_data;
}

static void on_listener_stop_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    ListenerPageContext *ctx = user_data;
    listener_manager_stop(ctx->backend->listener_system->listener_manager, ctx->listener_id);
}

static void on_listener_restart_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    ListenerPageContext *ctx = user_data;
    listener_manager_restart(ctx->backend->listener_system->listener_manager, ctx->listener_id);
}

/* Re-queries the registry and updates a listener page's labels and
 * Stop/Restart sensitivity. Called once when the page is first built
 * (so it never shows blank/stale text) and again every tick thereafter
 * (see on_tick) for as long as the tab stays open. */
static void refresh_listener_page(GtkBackend *backend, GtkWidget *page, uint64_t listener_id) {
    GtkWidget *endpoint_label = g_object_get_data(G_OBJECT(page), "workbench-listener-endpoint-label");
    GtkWidget *state_label = g_object_get_data(G_OBJECT(page), "workbench-listener-state-label");
    GtkWidget *stop_button = g_object_get_data(G_OBJECT(page), "workbench-listener-stop-button");
    GtkWidget *restart_button = g_object_get_data(G_OBJECT(page), "workbench-listener-restart-button");

    const Listener *listener = object_registry_get_listener(backend->listener_system->registry, listener_id);
    if (!listener) {
        gtk_label_set_text(GTK_LABEL(state_label), "State: gone");
        gtk_widget_set_sensitive(stop_button, FALSE);
        gtk_widget_set_sensitive(restart_button, FALSE);
        return;
    }

    char endpoint_text[160];
    /* narrow form: "<TCP|HTTP> • host:port" - always the callback
     * endpoint (never bind_address, which may be a 0.0.0.0 wildcard). */
    snprintf(endpoint_text, sizeof(endpoint_text), "%s \xE2\x80\xA2 %s:%u",
             listener_type_label(listener->config.type), listener->config.callback_host, listener->config.port);
    gtk_label_set_text(GTK_LABEL(endpoint_label), endpoint_text);

    char state_text[224];
    if (listener->runtime.state == LISTENER_STATE_ERROR && listener->runtime.last_error) {
        snprintf(state_text, sizeof(state_text), "State: %s (%s)", listener_state_name(listener->runtime.state),
                 listener->runtime.last_error);
    } else {
        snprintf(state_text, sizeof(state_text), "State: %s", listener_state_name(listener->runtime.state));
    }
    gtk_label_set_text(GTK_LABEL(state_label), state_text);

    ManagedObject obj;
    obj.type = MANAGED_OBJECT_LISTENER;
    obj.listener = *listener;
    gtk_widget_set_sensitive(stop_button, object_can_stop(&obj));
    gtk_widget_set_sensitive(restart_button, object_can_restart(&obj));
}

GtkWidget *build_listener_page(GtkBackend *backend, Tab *tab) {
    uint64_t listener_id = listener_tab_id(tab);

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);

    GtkWidget *endpoint_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(endpoint_label), 0.0);
    gtk_box_pack_start(GTK_BOX(page), endpoint_label, FALSE, FALSE, 0);

    GtkWidget *state_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state_label), 0.0);
    gtk_box_pack_start(GTK_BOX(page), state_label, FALSE, FALSE, 0);

    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *stop_button = gtk_button_new_with_label("Stop");
    GtkWidget *restart_button = gtk_button_new_with_label("Restart");
    gtk_box_pack_start(GTK_BOX(button_box), stop_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_box), restart_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), button_box, FALSE, FALSE, 0);

    /* Tagged on the page itself (not a global find-by-tag) so this
     * scales correctly once more than one listener tab is open at once. */
    g_object_set_data(G_OBJECT(page), "workbench-listener-endpoint-label", endpoint_label);
    g_object_set_data(G_OBJECT(page), "workbench-listener-state-label", state_label);
    g_object_set_data(G_OBJECT(page), "workbench-listener-stop-button", stop_button);
    g_object_set_data(G_OBJECT(page), "workbench-listener-restart-button", restart_button);

    ListenerPageContext *ctx = g_new(ListenerPageContext, 1);
    ctx->backend = backend;
    ctx->listener_id = listener_id;
    g_object_set_data_full(G_OBJECT(page), "workbench-listener-page-context", ctx, g_free);

    g_signal_connect(stop_button, "clicked", G_CALLBACK(on_listener_stop_clicked), ctx);
    g_signal_connect(restart_button, "clicked", G_CALLBACK(on_listener_restart_clicked), ctx);

    refresh_listener_page(backend, page, listener_id);
    return page;
}

static GtkWidget *find_listener_page_widget(GtkBackend *backend, uint64_t listener_id) {
    if (!backend->notebook) {
        return NULL;
    }
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
        if (tab && tab->type == TAB_TYPE_LISTENER && listener_tab_id(tab) == listener_id) {
            return page;
        }
    }
    return NULL;
}

gboolean has_listener_tab(GtkBackend *backend, uint64_t listener_id) {
    return find_listener_page_widget(backend, listener_id) != NULL;
}

/* Called from on_tick on LISTENER_EVENT_STARTED - see the "Listener
 * console tab" section above for why the tab only opens once bound,
 * and why it doesn't steal focus (add_tab_page's focus=FALSE). */
void open_listener_tab(GtkBackend *backend, uint64_t listener_id) {
    const Listener *listener = object_registry_get_listener(backend->listener_system->registry, listener_id);
    if (!listener) {
        return;
    }

    Workspace *workspace = workbench_get_workspace(backend->workbench);
    char title[192];
    /* "<name> — <callback>:<port>" - em dash, always the callback
     * endpoint, never bind_address (which may be a 0.0.0.0 wildcard). */
    g_snprintf(title, sizeof(title), "%s \xE2\x80\x94 %s:%u", listener->config.name, listener->config.callback_host,
               listener->config.port);

    Tab *tab = tab_create(TAB_TYPE_LISTENER, title);
    tab->backend_data = (void *)(uintptr_t)listener_id;
    workspace_add_tab(workspace, tab);
    add_tab_page(backend, tab, FALSE);
}

/* Object panel row-activation (a listener row) needs "focus if open,
 * else create" - open_listener_tab above only ever auto-opens once and
 * skips duplicates, it never focuses an already-open tab on demand. */
void focus_or_open_listener_tab(GtkBackend *backend, uint64_t listener_id) {
    GtkWidget *page = find_listener_page_widget(backend, listener_id);
    if (page) {
        focus_page(backend, page);
        return;
    }
    open_listener_tab(backend, listener_id);
    page = find_listener_page_widget(backend, listener_id);
    if (page) {
        focus_page(backend, page);
    }
}

void refresh_all_listener_tabs(GtkBackend *backend) {
    if (!backend->notebook) {
        return;
    }
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
        if (tab && tab->type == TAB_TYPE_LISTENER) {
            refresh_listener_page(backend, page, listener_tab_id(tab));
        }
    }
}
/* --- end Listener console tab ----------------------------------------- */
