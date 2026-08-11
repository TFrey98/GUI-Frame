#include "ui_gtk_backend.h"
#include "ui_gtk_tabs_internal.h"
#include "ui_gtk_terminal_internal.h"

#include <stdio.h>
#include <string.h>

/* --- Connection terminal view -----------------------------------------
 * A TerminalView bound to a connection_id rather than a locally spawned
 * shell: no PTY, no terminal_start_shell() - retained history plus live
 * incoming bytes are fed for display only (terminal_feed_output()), and
 * whatever the user types is forwarded to connection_manager_send()
 * instead of a child process (terminal_set_commit_handler()). Closing
 * the tab detaches only: on_tab_close_clicked needs zero changes, same
 * as the listener tab - it already falls through to "just remove the
 * page," and removing the page is what triggers this context's
 * destroy-notify below, which owns the only per-view resource (view). */

static uint64_t connection_tab_id(const Tab *tab) {
    /* Connection tabs use Tab.backend_data as an integer-sized opaque
     * handle, not as an owned pointer. Keep both casts: uintptr_t is the
     * portable bridge between void * and an integer that can hold it. */
    return (uint64_t)(uintptr_t)tab->backend_data;
}

/* One interactive writer per connection (the doc's single-writer
 * invariant); every other open view for the same connection is
 * read-only until it takes control. Tracked per view, here - never on
 * the registry's Connection, which has no concept of "views" by
 * design. page is a back-reference to the widget this context is
 * attached to, so a button click handler that only has ctx can still
 * tell take_control() which page just won. */
typedef struct ConnectionPageContext {
    GtkBackend *backend;
    GtkWidget *page;
    uint64_t connection_id;
    size_t history_offset; /* how much of TerminalHistory has been fed so far */
    Terminal *view;
    gboolean is_writer;
} ConnectionPageContext;

static void destroy_connection_page_context(gpointer data) {
    ConnectionPageContext *ctx = data;
    /* Registered with g_object_set_data_full() below, so GTK invokes this
     * when the page is finalized. The registry owns the Connection and its
     * history; this page owns only its presentation-side Terminal. */
    terminal_destroy(ctx->view);
    g_free(ctx);
}

/* The TerminalCommitHandler - fires with whatever the user typed,
 * whether or not this view currently holds control. No local echo: a
 * real remote shell already echoes its own input back over the wire,
 * same as nc's own behavior. */
static void on_connection_commit(const char *data, size_t length, void *user_data) {
    ConnectionPageContext *ctx = user_data;
    if (!ctx->is_writer) {
        return;
    }
    const Connection *connection =
        object_registry_get_connection(ctx->backend->listener_system->registry, ctx->connection_id);
    if (!connection || connection->state != CONNECTION_STATE_CONNECTED) {
        return;
    }
    connection_manager_send(ctx->backend->listener_system->connection_manager, ctx->connection_id, data, length);
}

/* Re-queries the registry, updates the state/control labels, feeds any
 * bytes appended to history since the last call, and updates
 * Take-Control sensitivity. Called once when the page is first built
 * (history_offset starts at 0, so this first call feeds all retained
 * history in one shot - no separate "initial dump" path needed) and
 * again every tick (see on_tick). */
static void refresh_connection_terminal_page(GtkBackend *backend, GtkWidget *page, ConnectionPageContext *ctx) {
    GtkWidget *state_label = g_object_get_data(G_OBJECT(page), "toolbox-connection-state-label");
    GtkWidget *control_label = g_object_get_data(G_OBJECT(page), "toolbox-connection-control-label");
    GtkWidget *take_control_button = g_object_get_data(G_OBJECT(page), "toolbox-connection-take-control-button");

    const Connection *connection =
        object_registry_get_connection(backend->listener_system->registry, ctx->connection_id);
    if (!connection) {
        /* A tab may outlive the registry object it was displaying. Leave
         * already-rendered output visible, but prevent any further input. */
        gtk_label_set_text(GTK_LABEL(state_label), "State: gone");
        gtk_widget_set_sensitive(take_control_button, FALSE);
        return;
    }

    char state_text[64];
    snprintf(state_text, sizeof(state_text), "State: %s", connection_state_name(connection->state));
    gtk_label_set_text(GTK_LABEL(state_label), state_text);

    size_t total = terminal_history_len(connection->history);
    if (total > ctx->history_offset) {
        /* Read in bounded chunks because history is append-only and can be
         * much larger than a single UI update should place on the stack.
         * history_offset is per page, which lets late-opened views replay
         * the complete session without affecting existing views. */
        char buf[4096];
        size_t copied = terminal_history_read(connection->history, ctx->history_offset, buf, sizeof(buf));
        while (copied > 0) {
            terminal_feed_output(ctx->view, buf, copied);
            ctx->history_offset += copied;
            copied = terminal_history_read(connection->history, ctx->history_offset, buf, sizeof(buf));
        }
    }

    gboolean connected = connection->state == CONNECTION_STATE_CONNECTED;
    gtk_label_set_text(GTK_LABEL(control_label), ctx->is_writer ? "You have control" : "Read-only");
    gtk_widget_set_sensitive(take_control_button, connected && !ctx->is_writer);
}

/* Calls fn(page, ctx, user_data) for every currently open
 * TAB_TYPE_CONNECTION_TERMINAL page bound to connection_id - shared by
 * take_control() (needs every page for one connection) and
 * refresh_all_connection_terminal_pages() (every connection-terminal
 * page, passing connection_id 0 to mean "all"). */
static void for_each_connection_terminal_page(GtkBackend *backend, uint64_t connection_id,
                                               void (*fn)(GtkWidget *page, ConnectionPageContext *ctx,
                                                          gpointer user_data),
                                               gpointer user_data) {
    if (!backend->notebook) {
        return;
    }
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
        if (!tab || tab->type != TAB_TYPE_CONNECTION_TERMINAL) {
            continue;
        }
        if (connection_id != 0 && connection_tab_id(tab) != connection_id) {
            continue;
        }
        ConnectionPageContext *ctx = g_object_get_data(G_OBJECT(page), "toolbox-connection-page-context");
        fn(page, ctx, user_data);
    }
}

static void set_writer_cb(GtkWidget *page, ConnectionPageContext *ctx, gpointer user_data) {
    GtkWidget *winning_page = user_data;
    ctx->is_writer = (page == winning_page);
    refresh_connection_terminal_page(ctx->backend, page, ctx);
}

static void take_control(GtkBackend *backend, uint64_t connection_id, GtkWidget *winning_page) {
    for_each_connection_terminal_page(backend, connection_id, set_writer_cb, winning_page);
}

static void on_take_control_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    ConnectionPageContext *ctx = user_data;
    take_control(ctx->backend, ctx->connection_id, ctx->page);
}

/* Defined after add_tab_page (it creates a new page) - forward declared
 * so build_connection_terminal_page below can wire it as a signal
 * handler and find_connection_terminal_page can decide whether a
 * brand-new page starts as the connection's writer. */
static void on_open_another_view_clicked(GtkButton *button, gpointer user_data);
static GtkWidget *find_connection_terminal_page(GtkBackend *backend, uint64_t connection_id);

GtkWidget *build_connection_terminal_page(GtkBackend *backend, Tab *tab) {
    uint64_t connection_id = connection_tab_id(tab);
    /* add_tab_page() has not appended this page to the notebook yet, so a
     * match here can only be an older view. The first view starts writable;
     * explicitly opened additional views start read-only. */
    gboolean already_open = find_connection_terminal_page(backend, connection_id) != NULL;

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);

    GtkWidget *state_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state_label), 0.0);
    gtk_box_pack_start(GTK_BOX(page), state_label, FALSE, FALSE, 0);

    GtkWidget *control_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(control_label), 0.0);
    gtk_box_pack_start(GTK_BOX(page), control_label, FALSE, FALSE, 0);

    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *take_control_button = gtk_button_new_with_label("Take Control");
    GtkWidget *open_another_button = gtk_button_new_with_label("Open Another View");
    gtk_box_pack_start(GTK_BOX(button_box), take_control_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_box), open_another_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), button_box, FALSE, FALSE, 0);

    Terminal *view = terminal_create();
    terminal_apply_theme(view, backend->dark_mode);
    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroller), terminal_get_widget(view));
    gtk_box_pack_start(GTK_BOX(page), scroller, TRUE, TRUE, 0);

    /* These are borrowed child-widget pointers. Attaching them to the page
     * avoids a second UI-only struct and is safe because GTK destroys the
     * children with their parent page. The string keys form a private
     * convention shared with the refresh path above. */
    g_object_set_data(G_OBJECT(page), "toolbox-connection-state-label", state_label);
    g_object_set_data(G_OBJECT(page), "toolbox-connection-control-label", control_label);
    g_object_set_data(G_OBJECT(page), "toolbox-connection-take-control-button", take_control_button);
    g_object_set_data(G_OBJECT(page), "toolbox-connection-open-another-button", open_another_button);
    g_object_set_data(G_OBJECT(page), "toolbox-view", view);

    ConnectionPageContext *ctx = g_new(ConnectionPageContext, 1);
    ctx->backend = backend;
    ctx->page = page;
    ctx->connection_id = connection_id;
    ctx->history_offset = 0;
    ctx->view = view;
    ctx->is_writer = !already_open;
    g_object_set_data_full(G_OBJECT(page), "toolbox-connection-page-context", ctx, destroy_connection_page_context);

    terminal_set_commit_handler(view, on_connection_commit, ctx);
    g_signal_connect(take_control_button, "clicked", G_CALLBACK(on_take_control_clicked), ctx);
    g_signal_connect(open_another_button, "clicked", G_CALLBACK(on_open_another_view_clicked), ctx);

    refresh_connection_terminal_page(backend, page, ctx);
    return page;
}
/* --- end Connection terminal view --------------------------------------- */

/* --- Connection terminal view (tab orchestration) -----------------------
 * The rest of this feature (page building, single-writer state) lives in
 * the "Connection terminal view" section above, before add_tab_page -
 * this half needs add_tab_page itself, so it has to come after. */

static GtkWidget *find_connection_terminal_page(GtkBackend *backend, uint64_t connection_id) {
    if (!backend->notebook) {
        return NULL;
    }
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i);
        /* add_tab_page() tags every notebook child with its model Tab. This
         * keeps lookup independent of tab titles, which users may rename. */
        Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
        if (tab && tab->type == TAB_TYPE_CONNECTION_TERMINAL && connection_tab_id(tab) == connection_id) {
            return page;
        }
    }
    return NULL;
}

/* Builds a brand-new page for connection_id, unconditionally - callers
 * decide whether dedup applies: open_or_focus_connection_terminal does,
 * on_open_another_view_clicked deliberately doesn't. No network
 * operation here either way - the connection's own worker (if any) is
 * untouched by a view attaching or detaching. */
static void open_new_connection_terminal_tab(GtkBackend *backend, uint64_t connection_id, gboolean focus) {
    const Connection *connection = object_registry_get_connection(backend->listener_system->registry, connection_id);
    if (!connection) {
        return;
    }
    Workspace *workspace = workbench_get_workspace(backend->workbench);
    char title[96];
    snprintf(title, sizeof(title), "%s:%u", connection->remote_host, connection->remote_port);

    Tab *tab = tab_create(TAB_TYPE_CONNECTION_TERMINAL, title);
    /* Unlike pointer-valued backend_data used by editor/local-terminal
     * tabs, this is a non-owning registry ID. close_tab_page() therefore
     * must not free or disconnect it. */
    tab->backend_data = (void *)(uintptr_t)connection_id;
    workspace_add_tab(workspace, tab);
    add_tab_page(backend, tab, focus);
}

/* workspace_reopen_terminal: focuses an existing view for connection_id
 * if one is open, otherwise attaches a new one. */
void open_or_focus_connection_terminal(GtkBackend *backend, uint64_t connection_id) {
    GtkWidget *page = find_connection_terminal_page(backend, connection_id);
    if (page) {
        focus_page(backend, page);
        return;
    }
    open_new_connection_terminal_tab(backend, connection_id, TRUE);
}

static void on_open_another_view_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    ConnectionPageContext *ctx = user_data;
    open_new_connection_terminal_tab(ctx->backend, ctx->connection_id, TRUE);
}

void refresh_all_connection_terminal_pages(GtkBackend *backend) {
    /* Called from the GTK main-loop tick after connection data has been
     * drained into TerminalHistory. Keeping all widget access here on the
     * GUI thread avoids synchronization inside Terminal and GTK. */
    if (!backend->notebook) {
        return;
    }
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
        if (tab && tab->type == TAB_TYPE_CONNECTION_TERMINAL) {
            ConnectionPageContext *ctx = g_object_get_data(G_OBJECT(page), "toolbox-connection-page-context");
            refresh_connection_terminal_page(backend, page, ctx);
        }
    }
}
/* --- end Connection terminal view (tab orchestration) --------------------- */
