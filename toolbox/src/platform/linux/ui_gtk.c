#include <gtk/gtk.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/tab.h"
#include "../../core/terminal_session.h"
#include "../../core/workspace.h"
#include "../../listeners/listener_system.h"
#include "../../listeners/object_predicates.h"
#include "../../tools/toolkit_index.h"
#include "../../ui/workbench.h"
#include "terminal_vte.h"

/* Tracks a live terminal tab's View+Session pair independent of the GTK
 * widget tree. The notebook and its pages are owned by the window and are
 * gone by the time platform_ui_destroy runs (the window closes and GTK
 * tears down its children during the run loop, well before app_destroy
 * gets to call us) - backend->notebook would be a dangling pointer at
 * that point, so shutdown cleanup walks this array instead. */
typedef struct TerminalEntry {
    Terminal *view;
    TerminalSession *session; /* not owned - the Tab owns it */
} TerminalEntry;

typedef struct GtkBackend {
    Workbench *workbench;
    GtkApplication *gtk_app;
    GtkWidget *notebook;
    int next_terminal_number; /* feeds the default "Terminal N" title; only
                                * ever increments, so closed numbers are not
                                * reused within a run. */
    GPtrArray *terminal_entries; /* TerminalEntry* */

    ListenerSystem *listener_system; /* borrowed from workbench */
    uint64_t last_listener_id;       /* most recently created listener; 0 = none yet */
    GtkWidget *status_label;         /* shows last_listener_id's name + state, via on_tick */
    int next_listener_number;        /* feeds the default "Listener N" name, mirrors
                                       * next_terminal_number above */
    GtkTreeStore *object_panel_store; /* bottom panel: listeners -> connections, see refresh_object_panel */
    guint tick_source_id;
} GtkBackend;

static void remove_terminal_entry(GtkBackend *backend, Terminal *view) {
    for (guint i = 0; i < backend->terminal_entries->len; i++) {
        TerminalEntry *entry = g_ptr_array_index(backend->terminal_entries, i);
        if (entry->view == view) {
            g_ptr_array_remove_index_fast(backend->terminal_entries, i);
            g_free(entry);
            return;
        }
    }
}

/* Shared by the listener console tab and the New Listener dialog's
 * status feedback. */
static const char *listener_state_name(ListenerState state) {
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
static const char *listener_type_label(ListenerType type) {
    switch (type) {
        case LISTENER_TYPE_REVERSE_TCP: return "TCP";
        case LISTENER_TYPE_HTTP: return "HTTP";
        case LISTENER_TYPE_HTTPS: return "HTTPS";
    }
    return "?";
}

/* Shared by the bottom object panel. */
static const char *connection_state_name(ConnectionState state) {
    switch (state) {
        case CONNECTION_STATE_CONNECTED: return "CONNECTED";
        case CONNECTION_STATE_DISCONNECTED: return "DISCONNECTED";
    }
    return "?";
}

/* Bottom object panel tree columns - declared here (rather than next to
 * build_bottom_panel/refresh_object_panel further down) so
 * on_object_panel_row_activated, defined earlier in the file as part of
 * the Connection terminal view section, can reference OBJECT_PANEL_COL_ID. */
enum {
    OBJECT_PANEL_COL_NAME,
    OBJECT_PANEL_COL_ENDPOINT,
    OBJECT_PANEL_COL_STATE,
    OBJECT_PANEL_COL_ID,
    OBJECT_PANEL_COL_COUNT
};

/* Shared by a tab label's click and entry-commit handlers so both can flip
 * between the display label and the editable rename entry for the same
 * tab. Owned by the event_box in build_tab_label via g_object_set_data_full,
 * so it's freed automatically when that widget is destroyed. */
typedef struct TabLabelData {
    Tab *tab;
    GtkWidget *label;
    GtkWidget *entry;
} TabLabelData;

/* Shared handler for both the sidebar and bottom-panel toggle buttons -
 * user_data is bound per-button to the specific panel widget it controls. */
static void on_toggle_panel(GtkToggleButton *button, gpointer user_data) {
    GtkWidget *panel = GTK_WIDGET(user_data);
    if (gtk_toggle_button_get_active(button)) {
        gtk_widget_show(panel);
    } else {
        gtk_widget_hide(panel);
    }
}

static void commit_rename(GtkEntry *entry, gpointer user_data) {
    TabLabelData *data = user_data;
    const char *text = gtk_entry_get_text(entry);
    if (text && *text) {
        tab_set_title(data->tab, text);
        gtk_label_set_text(GTK_LABEL(data->label), text);
        if (data->tab->type == TAB_TYPE_TERMINAL && data->tab->backend_data) {
            terminal_session_set_title((TerminalSession *)data->tab->backend_data, text);
        }
    }
    gtk_widget_hide(data->entry);
    gtk_widget_show(data->label);
}

static gboolean on_entry_focus_out(GtkWidget *entry, GdkEventFocus *event, gpointer user_data) {
    (void)event;
    commit_rename(GTK_ENTRY(entry), user_data);
    return FALSE;
}

static gboolean on_label_button_press(GtkWidget *event_box, GdkEventButton *event, gpointer user_data) {
    (void)event_box;
    TabLabelData *data = user_data;
    if (event->type == GDK_2BUTTON_PRESS && event->button == 1) {
        gtk_entry_set_text(GTK_ENTRY(data->entry), data->tab->title);
        gtk_widget_hide(data->label);
        gtk_widget_show(data->entry);
        gtk_widget_grab_focus(data->entry);
        gtk_editable_select_region(GTK_EDITABLE(data->entry), 0, -1);
        return TRUE;
    }
    return FALSE;
}

/* The actual close logic - shared by every tab type/state, unconditional
 * once called. The listener-running confirmation that can intercept
 * *before* this runs lives further down, in the real on_tab_close_clicked
 * definition after the "Listener console tab" section, since it needs
 * that section's ListenerPageContext/listener_tab_id. */
static void close_tab_page(GtkWidget *page) {
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    Workspace *workspace = g_object_get_data(G_OBJECT(page), "toolbox-workspace");
    GtkWidget *notebook = gtk_widget_get_ancestor(page, GTK_TYPE_NOTEBOOK);

    /* Captured before workspace_close_tab, which destroys *tab - tab_id and
     * page_num must survive that call for the workspace/notebook cleanup below. */
    uint64_t tab_id = tab->id;
    int page_num = gtk_notebook_page_num(GTK_NOTEBOOK(notebook), page);

    if (tab->type == TAB_TYPE_TERMINAL) {
        GtkBackend *backend = g_object_get_data(G_OBJECT(page), "toolbox-backend");
        Terminal *view = g_object_get_data(G_OBJECT(page), "toolbox-view");
        if (view) {
            remove_terminal_entry(backend, view);
            terminal_destroy(view);
        }
        if (tab->backend_data) {
            terminal_session_destroy((TerminalSession *)tab->backend_data);
            tab->backend_data = NULL;
        }
    }

    workspace_close_tab(workspace, tab_id);
    if (page_num >= 0) {
        gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), page_num);
    }
}

/* Forward declared for build_tab_label below - defined after the
 * "Listener console tab" section. */
static void on_tab_close_clicked(GtkButton *button, gpointer user_data);

/* Builds "Title  x" with the title double-click-to-rename and the x
 * closing the page. `page` must already carry "toolbox-tab"/
 * "toolbox-workspace" object data. */
static GtkWidget *build_tab_label(Tab *tab, GtkWidget *page) {
    GtkWidget *label = gtk_label_new(tab->title);

    GtkWidget *entry = gtk_entry_new();
    gtk_widget_set_no_show_all(entry, TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 10);

    GtkWidget *label_stack = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(label_stack), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(label_stack), entry, FALSE, FALSE, 0);

    GtkWidget *event_box = gtk_event_box_new();
    gtk_widget_add_events(event_box, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(event_box), label_stack);

    TabLabelData *data = g_new0(TabLabelData, 1);
    data->tab = tab;
    data->label = label;
    data->entry = entry;
    g_object_set_data_full(G_OBJECT(event_box), "toolbox-label-data", data, g_free);

    g_signal_connect(event_box, "button-press-event", G_CALLBACK(on_label_button_press), data);
    g_signal_connect(entry, "activate", G_CALLBACK(commit_rename), data);
    g_signal_connect(entry, "focus-out-event", G_CALLBACK(on_entry_focus_out), data);

    GtkWidget *close_button = gtk_button_new_with_label("×");
    gtk_button_set_relief(GTK_BUTTON(close_button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close_button, FALSE);
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_tab_close_clicked), page);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(box), event_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), close_button, FALSE, FALSE, 0);
    gtk_widget_show_all(box);

    return box;
}

/* --- Listener console tab --------------------------------------------
 * Opened automatically (see on_tick) once a listener's worker actually
 * confirms it's bound (LISTENER_EVENT_STARTED) - never at creation
 * time, so a listener that fails to bind never gets a dead tab.
 * Closing it is handled entirely by on_tab_close_clicked's existing
 * non-TERMINAL fallthrough - just removes the tab, never touches the
 * listener itself, satisfying invariant 4 with no extra code there. */

/* tab->backend_data holds the listener_id directly (cast through
 * uintptr_t - safe on this project's only target, 64-bit Linux, where
 * a pointer and a uint64_t are the same size), not an owned pointer
 * like TerminalSession* - there's no separate object for this tab to
 * own, ObjectRegistry already owns the real Listener. */
static uint64_t listener_tab_id(const Tab *tab) {
    return (uint64_t)(uintptr_t)tab->backend_data;
}

/* A single backend pointer (the user_data every other handler in this
 * file uses) isn't enough context for these two - they also need to
 * know *which* listener's page they belong to. */
typedef struct ListenerPageContext {
    GtkBackend *backend;
    uint64_t listener_id;
} ListenerPageContext;

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
    GtkWidget *endpoint_label = g_object_get_data(G_OBJECT(page), "toolbox-listener-endpoint-label");
    GtkWidget *state_label = g_object_get_data(G_OBJECT(page), "toolbox-listener-state-label");
    GtkWidget *stop_button = g_object_get_data(G_OBJECT(page), "toolbox-listener-stop-button");
    GtkWidget *restart_button = g_object_get_data(G_OBJECT(page), "toolbox-listener-restart-button");

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

static GtkWidget *build_listener_page(GtkBackend *backend, Tab *tab) {
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
    g_object_set_data(G_OBJECT(page), "toolbox-listener-endpoint-label", endpoint_label);
    g_object_set_data(G_OBJECT(page), "toolbox-listener-state-label", state_label);
    g_object_set_data(G_OBJECT(page), "toolbox-listener-stop-button", stop_button);
    g_object_set_data(G_OBJECT(page), "toolbox-listener-restart-button", restart_button);

    ListenerPageContext *ctx = g_new(ListenerPageContext, 1);
    ctx->backend = backend;
    ctx->listener_id = listener_id;
    g_object_set_data_full(G_OBJECT(page), "toolbox-listener-page-context", ctx, g_free);

    g_signal_connect(stop_button, "clicked", G_CALLBACK(on_listener_stop_clicked), ctx);
    g_signal_connect(restart_button, "clicked", G_CALLBACK(on_listener_restart_clicked), ctx);

    refresh_listener_page(backend, page, listener_id);
    return page;
}

static void on_close_running_listener_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    GtkWidget *page = user_data;
    if (response_id == GTK_RESPONSE_YES) { /* "Stop Listener" */
        ListenerPageContext *ctx = g_object_get_data(G_OBJECT(page), "toolbox-listener-page-context");
        listener_manager_stop(ctx->backend->listener_system->listener_manager, ctx->listener_id);
        close_tab_page(page);
    } else if (response_id == GTK_RESPONSE_ACCEPT) { /* "Close Tab" */
        close_tab_page(page);
    }
    /* Cancel or dismissing the dialog (GTK_RESPONSE_DELETE_EVENT): do
     * nothing - the tab stays open, exactly as if Close had never been
     * clicked. */
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

/* Guards closing a listener console tab specifically while its listener
 * is RUNNING; every other tab type/state closes exactly as
 * close_tab_page() alone always did - zero behavior change there. */
static void on_tab_close_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWidget *page = GTK_WIDGET(user_data);
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");

    if (tab->type == TAB_TYPE_LISTENER) {
        ListenerPageContext *ctx = g_object_get_data(G_OBJECT(page), "toolbox-listener-page-context");
        const Listener *listener =
            object_registry_get_listener(ctx->backend->listener_system->registry, ctx->listener_id);
        if (listener && listener->runtime.state == LISTENER_STATE_RUNNING) {
            GtkWindow *parent = gtk_application_get_active_window(ctx->backend->gtk_app);
            GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                                         GTK_BUTTONS_NONE,
                                                         "This listener is running. What would you like to do?");
            gtk_dialog_add_button(GTK_DIALOG(dialog), "Close Tab", GTK_RESPONSE_ACCEPT);
            gtk_dialog_add_button(GTK_DIALOG(dialog), "Stop Listener", GTK_RESPONSE_YES);
            gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
            g_signal_connect(dialog, "response", G_CALLBACK(on_close_running_listener_response), page);
            gtk_widget_show_all(dialog);
            return;
        }
    }
    close_tab_page(page);
}
/* --- end Listener console tab ----------------------------------------- */

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
        gtk_label_set_text(GTK_LABEL(state_label), "State: gone");
        gtk_widget_set_sensitive(take_control_button, FALSE);
        return;
    }

    char state_text[64];
    snprintf(state_text, sizeof(state_text), "State: %s", connection_state_name(connection->state));
    gtk_label_set_text(GTK_LABEL(state_label), state_text);

    size_t total = terminal_history_len(connection->history);
    if (total > ctx->history_offset) {
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

static GtkWidget *build_connection_terminal_page(GtkBackend *backend, Tab *tab) {
    uint64_t connection_id = connection_tab_id(tab);
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
    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroller), terminal_get_widget(view));
    gtk_box_pack_start(GTK_BOX(page), scroller, TRUE, TRUE, 0);

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

/* The Tab owns the TerminalSession (session identity - id, title, cwd,
 * shell path, running/exit state - independent of any widget); the page
 * widget carries the View (the live VteTerminal wrapper) as its own
 * object data, since the View is purely a platform-layer concern. */
static GtkWidget *build_terminal_page(GtkBackend *backend, Tab *tab) {
    TerminalSession *session = terminal_session_create(tab->id, tab->title);
    tab->backend_data = session;

    Terminal *view = terminal_create();
    if (terminal_start_shell(view, session) != 0) {
        g_printerr("toolbox: could not start a shell for tab '%s'\n", tab->title);
    }

    TerminalEntry *entry = g_new(TerminalEntry, 1);
    entry->view = view;
    entry->session = session;
    g_ptr_array_add(backend->terminal_entries, entry);

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroller), terminal_get_widget(view));
    g_object_set_data(G_OBJECT(scroller), "toolbox-view", view);
    g_object_set_data(G_OBJECT(scroller), "toolbox-backend", backend);
    return scroller;
}

/* focus controls whether the new page also becomes the active tab -
 * true for every explicit user action (clicking "+ Terminal", the
 * initial startup tab), false for a listener tab opening on its own
 * from a background tick (LISTENER_EVENT_STARTED can land at any time;
 * it shouldn't yank focus away from whatever the user is doing). */
/* Switches the notebook to page and grabs keyboard focus for its
 * Terminal view, if it has one - "toolbox-view" tags both terminal
 * kinds (local shell and connection-backed) identically, so this one
 * tag-based check covers both. Shared by add_tab_page's own focus
 * branch and the focus-or-open helpers below. */
static void focus_page(GtkBackend *backend, GtkWidget *page) {
    gint index = gtk_notebook_page_num(GTK_NOTEBOOK(backend->notebook), page);
    if (index < 0) {
        return;
    }
    gtk_notebook_set_current_page(GTK_NOTEBOOK(backend->notebook), index);
    Terminal *view = g_object_get_data(G_OBJECT(page), "toolbox-view");
    if (view) {
        gtk_widget_grab_focus(terminal_get_widget(view));
    }
}

static void add_tab_page(GtkBackend *backend, Tab *tab, gboolean focus) {
    Workspace *workspace = workbench_get_workspace(backend->workbench);

    GtkWidget *page;
    if (tab->type == TAB_TYPE_TERMINAL) {
        page = build_terminal_page(backend, tab);
    } else if (tab->type == TAB_TYPE_LISTENER) {
        page = build_listener_page(backend, tab);
    } else if (tab->type == TAB_TYPE_CONNECTION_TERMINAL) {
        page = build_connection_terminal_page(backend, tab);
    } else {
        page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_set_border_width(GTK_CONTAINER(page), 12);
        gtk_box_pack_start(GTK_BOX(page), gtk_label_new(tab->title), FALSE, FALSE, 0);
    }

    g_object_set_data(G_OBJECT(page), "toolbox-tab", tab);
    g_object_set_data(G_OBJECT(page), "toolbox-workspace", workspace);

    GtkWidget *label = build_tab_label(tab, page);

    gtk_notebook_append_page(GTK_NOTEBOOK(backend->notebook), page, label);
    gtk_widget_show_all(page);

    if (focus) {
        focus_page(backend, page);
    }
}

static GtkWidget *find_listener_page_widget(GtkBackend *backend, uint64_t listener_id) {
    if (!backend->notebook) {
        return NULL;
    }
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
        if (tab && tab->type == TAB_TYPE_LISTENER && listener_tab_id(tab) == listener_id) {
            return page;
        }
    }
    return NULL;
}

static gboolean has_listener_tab(GtkBackend *backend, uint64_t listener_id) {
    return find_listener_page_widget(backend, listener_id) != NULL;
}

/* Called from on_tick on LISTENER_EVENT_STARTED - see the "Listener
 * console tab" section above for why the tab only opens once bound,
 * and why it doesn't steal focus (add_tab_page's focus=FALSE). */
static void open_listener_tab(GtkBackend *backend, uint64_t listener_id) {
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
static void focus_or_open_listener_tab(GtkBackend *backend, uint64_t listener_id) {
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

static void refresh_all_listener_tabs(GtkBackend *backend) {
    if (!backend->notebook) {
        return;
    }
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
        if (tab && tab->type == TAB_TYPE_LISTENER) {
            refresh_listener_page(backend, page, listener_tab_id(tab));
        }
    }
}

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
    tab->backend_data = (void *)(uintptr_t)connection_id;
    workspace_add_tab(workspace, tab);
    add_tab_page(backend, tab, focus);
}

/* workspace_reopen_terminal: focuses an existing view for connection_id
 * if one is open, otherwise attaches a new one. */
static void open_or_focus_connection_terminal(GtkBackend *backend, uint64_t connection_id) {
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

static void refresh_all_connection_terminal_pages(GtkBackend *backend) {
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
/* --- end Connection terminal view (tab orchestration) --------------------- */

static void on_add_tab_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkBackend *backend = user_data;
    Workspace *workspace = workbench_get_workspace(backend->workbench);

    char title[32];
    g_snprintf(title, sizeof(title), "Terminal %d", backend->next_terminal_number++);

    Tab *tab = tab_create(TAB_TYPE_TERMINAL, title);
    workspace_add_tab(workspace, tab);
    add_tab_page(backend, tab, TRUE);
}

static void on_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
    (void)notebook;
    (void)page_num;
    Workspace *workspace = user_data;
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    if (!tab) {
        return;
    }
    workspace_set_active_tab(workspace, tab->id);
    if (tab->type == TAB_TYPE_TERMINAL) {
        Terminal *view = g_object_get_data(G_OBJECT(page), "toolbox-view");
        if (view) {
            gtk_widget_grab_focus(terminal_get_widget(view));
        }
    }
}

enum {
    TOOLKIT_COL_ICON,
    TOOLKIT_COL_NAME,
    TOOLKIT_COL_PATH,
    TOOLKIT_COL_IS_DIR,
    TOOLKIT_COL_LOADED,
    TOOLKIT_COL_COUNT
};

/* Appends entry as a row under parent (NULL for the root). Directories get
 * a single placeholder child so their expander arrow shows immediately;
 * the placeholder is replaced with real children lazily in
 * on_toolkit_row_expanded, once the row is actually expanded. */
static void add_toolkit_tree_entry(GtkTreeStore *store, GtkTreeIter *parent, const ToolkitEntry *entry) {
    GtkTreeIter iter;
    gtk_tree_store_append(store, &iter, parent);
    gtk_tree_store_set(store, &iter,
        TOOLKIT_COL_ICON, entry->is_dir ? "folder" : "text-x-generic",
        TOOLKIT_COL_NAME, entry->name,
        TOOLKIT_COL_PATH, entry->path,
        TOOLKIT_COL_IS_DIR, entry->is_dir,
        TOOLKIT_COL_LOADED, FALSE,
        -1);

    if (entry->is_dir) {
        GtkTreeIter placeholder;
        gtk_tree_store_append(store, &placeholder, &iter);
        gtk_tree_store_set(store, &placeholder, TOOLKIT_COL_NAME, "", -1);
    }
}

/* Clears and rebuilds the root level of store from the current
 * toolkit_index state. Called at sidebar construction and again by the
 * refresh button; any expanded subfolders collapse back to unloaded. */
static void populate_toolkit_tree(GtkTreeStore *store) {
    gtk_tree_store_clear(store);
    int count = toolkit_index_count();
    for (int i = 0; i < count; i++) {
        add_toolkit_tree_entry(store, NULL, toolkit_index_get(i));
    }
}

/* Lazily scans a directory row's contents the first time it's expanded,
 * replacing its placeholder child with the real (still non-recursive -
 * only this one level) listing. Re-expanding after a collapse reuses what
 * was already loaded rather than re-scanning. */
static void on_toolkit_row_expanded(GtkTreeView *tree_view, GtkTreeIter *iter, GtkTreePath *tree_path, gpointer user_data) {
    (void)tree_path;
    (void)user_data;
    GtkTreeStore *store = GTK_TREE_STORE(gtk_tree_view_get_model(tree_view));

    gboolean loaded = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(store), iter, TOOLKIT_COL_LOADED, &loaded, -1);
    if (loaded) {
        return;
    }

    char *dir_path = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(store), iter, TOOLKIT_COL_PATH, &dir_path, -1);

    /* Keep the placeholder in place while appending the real children -
     * GtkTreeView auto-collapses a row that transitions through having
     * zero children, so removing it before adding the replacements would
     * silently undo the expansion that's currently in progress. */
    GtkTreeIter placeholder;
    gboolean has_placeholder = gtk_tree_model_iter_children(GTK_TREE_MODEL(store), &placeholder, iter);

    if (dir_path) {
        ToolkitEntry entries[TOOLKIT_INDEX_MAX_ENTRIES];
        int n = toolkit_scan_directory(dir_path, entries, TOOLKIT_INDEX_MAX_ENTRIES);
        for (int i = 0; i < n; i++) {
            add_toolkit_tree_entry(store, iter, &entries[i]);
            free(entries[i].name);
            free(entries[i].path);
        }
    }
    g_free(dir_path);

    if (has_placeholder) {
        gtk_tree_store_remove(store, &placeholder);
    }

    gtk_tree_store_set(store, iter, TOOLKIT_COL_LOADED, TRUE, -1);
}

static void on_toolkit_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkTreeStore *store = GTK_TREE_STORE(user_data);
    toolkit_index_rescan();
    populate_toolkit_tree(store);
}

/* Double-click (or Enter) on a directory row toggles it, in addition to
 * the expander arrow GtkTreeView already handles on a single click. */
static void on_toolkit_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data) {
    (void)column;
    (void)user_data;
    if (gtk_tree_view_row_expanded(tree_view, path)) {
        gtk_tree_view_collapse_row(tree_view, path);
    } else {
        gtk_tree_view_expand_row(tree_view, path, FALSE);
    }
}

static GtkWidget *build_sidebar(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(box, 200, -1);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *title = gtk_label_new("Toolkit");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    GtkWidget *refresh_button = gtk_button_new_with_label("\xE2\x86\xBB"); /* refresh: ↻ */
    gtk_button_set_relief(GTK_BUTTON(refresh_button), GTK_RELIEF_NONE);
    gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(header), refresh_button, FALSE, FALSE, 0);

    GtkTreeStore *store = gtk_tree_store_new(TOOLKIT_COL_COUNT,
        G_TYPE_STRING,  /* icon name */
        G_TYPE_STRING,  /* display name */
        G_TYPE_STRING,  /* full path */
        G_TYPE_BOOLEAN, /* is_dir */
        G_TYPE_BOOLEAN  /* loaded */
    );
    populate_toolkit_tree(store);

    GtkWidget *tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store); /* the tree view holds its own reference */
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree_view), FALSE);

    GtkTreeViewColumn *column = gtk_tree_view_column_new();
    GtkCellRenderer *icon_renderer = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon_renderer, "stock-size", GTK_ICON_SIZE_MENU, NULL);
    gtk_tree_view_column_pack_start(column, icon_renderer, FALSE);
    gtk_tree_view_column_add_attribute(column, icon_renderer, "icon-name", TOOLKIT_COL_ICON);

    GtkCellRenderer *text_renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(column, text_renderer, TRUE);
    gtk_tree_view_column_add_attribute(column, text_renderer, "text", TOOLKIT_COL_NAME);

    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);
    g_signal_connect(tree_view, "row-expanded", G_CALLBACK(on_toolkit_row_expanded), NULL);
    g_signal_connect(tree_view, "row-activated", G_CALLBACK(on_toolkit_row_activated), NULL);

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), tree_view);

    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_toolkit_refresh_clicked), store);

    gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);

    return box;
}

/* --- Bottom object panel -----------------------------------------------
 * Tree from the registry: listeners -> their connections. Refreshed every
 * tick (see on_tick/refresh_object_panel) rather than on a manual action
 * like the Toolkit sidebar's refresh button, since the checkpoint requires
 * connections to appear live as they connect. That live-refresh
 * requirement is why this syncs rows in place instead of the sidebar's
 * clear-and-repopulate: clearing every 100ms would collapse any row the
 * user had expanded on every single tick.
 *
 * The sync below only ever updates a row's text in place or appends a new
 * row - it never removes one. That's safe today because nothing in this
 * file ever calls object_registry_remove() (guarded removal is Phase 10's
 * job); revisit this once a remove action exists. See the top of this
 * file for OBJECT_PANEL_COL_*'s declaration. */

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

/* Bidirectional as of Phase 10 (guarded remove): pass 1 walks the
 * store's existing children, updating any whose connection still
 * exists and removing any that don't (gtk_tree_store_remove() returns
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
static void refresh_object_panel(GtkBackend *backend) {
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
static void popup_object_context_menu(GtkBackend *backend, GtkWidget *tree_view, GtkTreePath *path,
                                       GdkEventButton *event) {
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

static gboolean on_object_panel_button_press(GtkWidget *tree_view, GdkEventButton *event, gpointer user_data) {
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
static gboolean on_object_panel_popup_menu(GtkWidget *tree_view, gpointer user_data) {
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

static GtkWidget *build_bottom_panel(GtkBackend *backend) {
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
    g_object_set_data(G_OBJECT(tree_view), "toolbox-object-panel-tree", tree_view);
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

/* --- New Listener dialog --------------------------------------------
 * Collects a real ListenerConfig and validates it (listener_config_validate())
 * before ever calling into ListenerManager, so invalid input never
 * reaches it - only a config that already passed gets handed to
 * listener_manager_create_listener(). Only Reverse TCP is exposed:
 * that's the only type listener_config_validate() currently accepts,
 * so Type is a plain label here, not a live control - the doc's
 * "conditional field sets per type"/"per-field dirty flags for type
 * switching" have nothing to switch between yet and are deferred to
 * whichever phase adds a second real type (HTTP). The one dirty-flag
 * behavior that *is* useful today: Callback Host auto-follows Bind
 * Address until the user edits Callback Host directly. */

typedef struct ListenerDialogState {
    GtkBackend *backend;
    GtkWidget *name_entry, *name_error;
    GtkWidget *type_combo; /* index 0 = Reverse TCP, 1 = HTTP, 2 = HTTPS */
    GtkWidget *bind_address_entry, *bind_address_error;
    GtkWidget *port_entry, *port_error;
    GtkWidget *callback_host_entry, *callback_host_error;
    /* HTTP or HTTPS only - all four hidden otherwise, including the row
     * labels (add_form_row's own label isn't otherwise reachable to
     * toggle). */
    GtkWidget *url_path_label, *url_path_entry, *url_path_error;
    GtkWidget *host_header_label, *host_header_entry, *host_header_error;
    /* HTTPS only - same hidden-unless-relevant treatment. */
    GtkWidget *cert_path_label, *cert_path_entry, *cert_path_error;
    GtkWidget *key_path_label, *key_path_entry, *key_path_error;
    GtkWidget *general_error; /* ENDPOINT and any other whole-config error */
    gboolean callback_host_dirty;    /* true once the user has typed into it directly */
    gboolean updating_callback_host; /* guards the auto-follow's own programmatic set
                                       * from being mistaken for a user edit */
    gboolean submitting; /* re-entrancy guard - not structurally reachable in this
                           * single-threaded, non-nested-loop GTK flow, but cheap
                           * insurance per the doc's explicit ask */
} ListenerDialogState;

static void on_bind_address_changed(GtkEditable *editable, gpointer user_data) {
    (void)editable;
    ListenerDialogState *state = user_data;
    if (state->callback_host_dirty) {
        return;
    }
    const char *bind_text = gtk_entry_get_text(GTK_ENTRY(state->bind_address_entry));
    state->updating_callback_host = TRUE;
    gtk_entry_set_text(GTK_ENTRY(state->callback_host_entry), bind_text);
    state->updating_callback_host = FALSE;
}

static void on_callback_host_changed(GtkEditable *editable, gpointer user_data) {
    (void)editable;
    ListenerDialogState *state = user_data;
    if (!state->updating_callback_host) {
        state->callback_host_dirty = TRUE;
    }
}

/* URL Path/Host Header apply to HTTP or HTTPS; Certificate/Key Path
 * apply to HTTPS only - shown only when relevant to the selected type,
 * the "conditional field sets per type" Phase 6 deferred since there
 * was nothing to switch between yet. */
static void on_listener_type_changed(GtkComboBox *combo, gpointer user_data) {
    ListenerDialogState *state = user_data;
    gint active = gtk_combo_box_get_active(combo);
    gboolean is_http_family = active == 1 || active == 2;
    gboolean is_https = active == 2;
    gtk_widget_set_visible(state->url_path_label, is_http_family);
    gtk_widget_set_visible(state->url_path_entry, is_http_family);
    gtk_widget_set_visible(state->url_path_error, is_http_family);
    gtk_widget_set_visible(state->host_header_label, is_http_family);
    gtk_widget_set_visible(state->host_header_entry, is_http_family);
    gtk_widget_set_visible(state->host_header_error, is_http_family);
    gtk_widget_set_visible(state->cert_path_label, is_https);
    gtk_widget_set_visible(state->cert_path_entry, is_https);
    gtk_widget_set_visible(state->cert_path_error, is_https);
    gtk_widget_set_visible(state->key_path_label, is_https);
    gtk_widget_set_visible(state->key_path_entry, is_https);
    gtk_widget_set_visible(state->key_path_error, is_https);
}

/* Non-numeric or out-of-range text both become 0, which
 * listener_config_validate() already flags as "Port must be between 1
 * and 65535" - no separate UI-side numeric check needed. */
static uint16_t parse_port(const char *text) {
    if (!text || !*text) {
        return 0;
    }
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 1 || value > 65535) {
        return 0;
    }
    return (uint16_t)value;
}

static GtkWidget *error_label_for_field(ListenerDialogState *state, ListenerConfigField field) {
    switch (field) {
        case LISTENER_CONFIG_FIELD_NAME: return state->name_error;
        case LISTENER_CONFIG_FIELD_BIND_ADDRESS: return state->bind_address_error;
        case LISTENER_CONFIG_FIELD_PORT: return state->port_error;
        case LISTENER_CONFIG_FIELD_CALLBACK_HOST: return state->callback_host_error;
        case LISTENER_CONFIG_FIELD_URL_PATH: return state->url_path_error;
        case LISTENER_CONFIG_FIELD_HOST_HEADER: return state->host_header_error;
        case LISTENER_CONFIG_FIELD_CERT_PATH: return state->cert_path_error;
        case LISTENER_CONFIG_FIELD_KEY_PATH: return state->key_path_error;
        default:
            /* TYPE/ENDPOINT: no dedicated widget in this dialog - TYPE
             * never fires since the combo only ever offers types
             * validate() accepts. */
            return state->general_error;
    }
}

static void clear_dialog_errors(ListenerDialogState *state) {
    gtk_label_set_text(GTK_LABEL(state->name_error), "");
    gtk_label_set_text(GTK_LABEL(state->bind_address_error), "");
    gtk_label_set_text(GTK_LABEL(state->port_error), "");
    gtk_label_set_text(GTK_LABEL(state->callback_host_error), "");
    gtk_label_set_text(GTK_LABEL(state->url_path_error), "");
    gtk_label_set_text(GTK_LABEL(state->host_header_error), "");
    gtk_label_set_text(GTK_LABEL(state->cert_path_error), "");
    gtk_label_set_text(GTK_LABEL(state->key_path_error), "");
    gtk_label_set_text(GTK_LABEL(state->general_error), "");
}

static void on_listener_dialog_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    ListenerDialogState *state = user_data;

    if (response_id != GTK_RESPONSE_OK) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }
    if (state->submitting) {
        return;
    }
    state->submitting = TRUE;
    clear_dialog_errors(state);

    gint active = gtk_combo_box_get_active(GTK_COMBO_BOX(state->type_combo));
    gboolean is_http_family = active == 1 || active == 2;
    gboolean is_https = active == 2;

    ListenerConfig config = {0};
    config.name = strdup(gtk_entry_get_text(GTK_ENTRY(state->name_entry)));
    config.type = is_https ? LISTENER_TYPE_HTTPS : (is_http_family ? LISTENER_TYPE_HTTP : LISTENER_TYPE_REVERSE_TCP);
    config.bind_address = strdup(gtk_entry_get_text(GTK_ENTRY(state->bind_address_entry)));
    config.port = parse_port(gtk_entry_get_text(GTK_ENTRY(state->port_entry)));
    config.callback_host = strdup(gtk_entry_get_text(GTK_ENTRY(state->callback_host_entry)));
    if (is_http_family) {
        config.url_path = strdup(gtk_entry_get_text(GTK_ENTRY(state->url_path_entry)));
        config.host_header = strdup(gtk_entry_get_text(GTK_ENTRY(state->host_header_entry)));
    }
    if (is_https) {
        config.cert_path = strdup(gtk_entry_get_text(GTK_ENTRY(state->cert_path_entry)));
        config.key_path = strdup(gtk_entry_get_text(GTK_ENTRY(state->key_path_entry)));
    }

    ObjectRegistry *registry = state->backend->listener_system->registry;
    ListenerConfigValidation validation;
    if (!listener_config_validate(registry, &config, &validation)) {
        for (int i = 0; i < validation.error_count; i++) {
            gtk_label_set_text(GTK_LABEL(error_label_for_field(state, validation.errors[i].field)),
                                validation.errors[i].message);
        }
        free(config.name);
        free(config.bind_address);
        free(config.callback_host);
        free(config.url_path);
        free(config.host_header);
        free(config.cert_path);
        free(config.key_path);
        state->submitting = FALSE;
        return; /* leave the dialog open so the user can fix it */
    }

    uint64_t id = listener_manager_create_listener(state->backend->listener_system->listener_manager, config, NULL);
    if (id == 0) {
        /* Shouldn't happen - nothing else can touch the registry between
         * the validate() call above and this one on this single-threaded
         * GTK flow - but don't silently do nothing if it somehow does. */
        gtk_label_set_text(GTK_LABEL(state->general_error), "Failed to create listener");
        free(config.name);
        free(config.bind_address);
        free(config.callback_host);
        free(config.url_path);
        free(config.host_header);
        free(config.cert_path);
        free(config.key_path);
        state->submitting = FALSE;
        return;
    }

    listener_manager_start_async(state->backend->listener_system->listener_manager, id);
    state->backend->last_listener_id = id;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

/* Returns the row's label widget - only URL Path/Host Header currently
 * need it (to hide the label along with its field for non-HTTP types);
 * every other call site just discards it. */
static GtkWidget *add_form_row(GtkGrid *grid, int row, const char *label_text, GtkWidget *entry) {
    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_grid_attach(grid, label, 0, row, 1, 1);
    gtk_grid_attach(grid, entry, 1, row, 1, 1);
    gtk_widget_set_hexpand(entry, TRUE);
    return label;
}

static GtkWidget *add_error_row(GtkGrid *grid, int row) {
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_grid_attach(grid, label, 1, row, 1, 1);
    return label;
}

static void open_new_listener_dialog(GtkBackend *backend, GtkWindow *parent) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons("New Listener", parent, GTK_DIALOG_MODAL, "_Cancel",
                                                      GTK_RESPONSE_CANCEL, "_Create", GTK_RESPONSE_OK, NULL);

    ListenerDialogState *state = g_new0(ListenerDialogState, 1);
    state->backend = backend;

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid, TRUE, TRUE, 0);

    int row = 0;

    state->name_entry = gtk_entry_new();
    char default_name[32];
    g_snprintf(default_name, sizeof(default_name), "Listener %d", backend->next_listener_number++);
    gtk_entry_set_text(GTK_ENTRY(state->name_entry), default_name);
    add_form_row(GTK_GRID(grid), row++, "Name", state->name_entry);
    state->name_error = add_error_row(GTK_GRID(grid), row++);

    state->type_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->type_combo), "Reverse TCP");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->type_combo), "HTTP");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->type_combo), "HTTPS");
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->type_combo), 0);
    add_form_row(GTK_GRID(grid), row++, "Type", state->type_combo);

    state->bind_address_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(state->bind_address_entry), "0.0.0.0");
    add_form_row(GTK_GRID(grid), row++, "Bind Address", state->bind_address_entry);
    state->bind_address_error = add_error_row(GTK_GRID(grid), row++);

    state->port_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(state->port_entry), "4444");
    add_form_row(GTK_GRID(grid), row++, "Port", state->port_entry);
    state->port_error = add_error_row(GTK_GRID(grid), row++);

    /* Left blank by default - Bind Address defaults to a wildcard, and
     * the auto-follow below deliberately never propagates a wildcard
     * into Callback Host (it's never a valid callback host). */
    state->callback_host_entry = gtk_entry_new();
    add_form_row(GTK_GRID(grid), row++, "Callback Host", state->callback_host_entry);
    state->callback_host_error = add_error_row(GTK_GRID(grid), row++);

    state->url_path_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(state->url_path_entry), "/");
    state->url_path_label = add_form_row(GTK_GRID(grid), row++, "URL Path", state->url_path_entry);
    state->url_path_error = add_error_row(GTK_GRID(grid), row++);

    state->host_header_entry = gtk_entry_new();
    state->host_header_label = add_form_row(GTK_GRID(grid), row++, "Host Header", state->host_header_entry);
    state->host_header_error = add_error_row(GTK_GRID(grid), row++);

    state->cert_path_entry = gtk_entry_new();
    state->cert_path_label = add_form_row(GTK_GRID(grid), row++, "Certificate Path", state->cert_path_entry);
    state->cert_path_error = add_error_row(GTK_GRID(grid), row++);

    state->key_path_entry = gtk_entry_new();
    state->key_path_label = add_form_row(GTK_GRID(grid), row++, "Private Key Path", state->key_path_entry);
    state->key_path_error = add_error_row(GTK_GRID(grid), row++);

    state->general_error = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->general_error), 0.0);
    gtk_grid_attach(GTK_GRID(grid), state->general_error, 0, row, 2, 1);

    /* Connected after every default gtk_entry_set_text() above, so
     * populating the defaults doesn't itself trigger the auto-follow. */
    g_signal_connect(state->bind_address_entry, "changed", G_CALLBACK(on_bind_address_changed), state);
    g_signal_connect(state->callback_host_entry, "changed", G_CALLBACK(on_callback_host_changed), state);
    g_signal_connect(state->type_combo, "changed", G_CALLBACK(on_listener_type_changed), state);
    g_signal_connect(dialog, "response", G_CALLBACK(on_listener_dialog_response), state);

    g_object_set_data(G_OBJECT(state->name_entry), "toolbox-listener-name-entry", state->name_entry);
    g_object_set_data(G_OBJECT(state->name_error), "toolbox-listener-name-error", state->name_error);
    g_object_set_data(G_OBJECT(state->type_combo), "toolbox-listener-type-combo", state->type_combo);
    g_object_set_data(G_OBJECT(state->bind_address_entry), "toolbox-listener-bind-address-entry",
                       state->bind_address_entry);
    g_object_set_data(G_OBJECT(state->port_entry), "toolbox-listener-port-entry", state->port_entry);
    g_object_set_data(G_OBJECT(state->callback_host_entry), "toolbox-listener-callback-host-entry",
                       state->callback_host_entry);
    g_object_set_data(G_OBJECT(state->url_path_entry), "toolbox-listener-url-path-entry", state->url_path_entry);
    g_object_set_data(G_OBJECT(state->host_header_entry), "toolbox-listener-host-header-entry",
                       state->host_header_entry);
    g_object_set_data(G_OBJECT(state->cert_path_entry), "toolbox-listener-cert-path-entry", state->cert_path_entry);
    g_object_set_data(G_OBJECT(state->key_path_entry), "toolbox-listener-key-path-entry", state->key_path_entry);
    g_object_set_data(G_OBJECT(dialog), "toolbox-new-listener-dialog", dialog);
    /* Frees state automatically when the dialog is destroyed - same
     * ownership pattern TabLabelData uses on its event_box above. */
    g_object_set_data_full(G_OBJECT(dialog), "toolbox-listener-dialog-state", state, g_free);

    gtk_widget_show_all(dialog);
    /* show_all() above forces every child visible, including the
     * HTTP-only rows - re-sync them to the combo's actual (default
     * Reverse TCP) selection now that showing is done. */
    on_listener_type_changed(GTK_COMBO_BOX(state->type_combo), state);
}

static void on_new_listener_clicked(GtkButton *button, gpointer user_data) {
    GtkBackend *backend = user_data;
    GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(button));
    open_new_listener_dialog(backend, GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL);
}
/* --- end New Listener dialog ----------------------------------------- */

/* g_timeout_add callback: the GUI's per-tick event pump. Never does
 * socket I/O itself - only ever reads registry state and drives the
 * managers, exactly like every headless test since Phase 3. */
static gboolean on_tick(gpointer user_data) {
    GtkBackend *backend = user_data;
    /* Window already torn down (see on_window_destroy) - nothing left to
     * update. Every downstream function here (has_listener_tab,
     * refresh_all_listener_tabs, refresh_all_connection_terminal_pages,
     * refresh_object_panel, ...) dereferences backend->notebook or
     * backend->object_panel_store unconditionally, so this one guard
     * covers all of them instead of each needing its own. */
    if (!backend->notebook) {
        return G_SOURCE_CONTINUE;
    }

    ListenerEvent events[32];
    int n = listener_system_pump(backend->listener_system, events, 32);
    for (int i = 0; i < n; i++) {
        if (events[i].type == LISTENER_EVENT_STARTED && !has_listener_tab(backend, events[i].object_id)) {
            open_listener_tab(backend, events[i].object_id);
        }
    }
    refresh_all_listener_tabs(backend);
    refresh_all_connection_terminal_pages(backend);
    refresh_object_panel(backend);

    if (backend->last_listener_id != 0 && backend->status_label) {
        const Listener *listener =
            object_registry_get_listener(backend->listener_system->registry, backend->last_listener_id);
        char text[192];
        if (!listener) {
            snprintf(text, sizeof(text), "Listener: gone");
        } else if (listener->runtime.state == LISTENER_STATE_ERROR && listener->runtime.last_error) {
            snprintf(text, sizeof(text), "%s: %s (%s)", listener->config.name,
                     listener_state_name(listener->runtime.state), listener->runtime.last_error);
        } else {
            snprintf(text, sizeof(text), "%s: %s", listener->config.name,
                     listener_state_name(listener->runtime.state));
        }
        gtk_label_set_text(GTK_LABEL(backend->status_label), text);
    }

    return G_SOURCE_CONTINUE;
}

static GtkWidget *build_top_bar(GtkBackend *backend, GtkWidget *sidebar, GtkWidget *bottom_panel) {
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 6);

    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("toolbox"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new(NULL), TRUE, TRUE, 0);

    GtkWidget *new_listener_button = gtk_button_new_with_label("+ New Listener");
    g_object_set_data(G_OBJECT(new_listener_button), "toolbox-new-listener-button", new_listener_button);
    g_signal_connect(new_listener_button, "clicked", G_CALLBACK(on_new_listener_clicked), backend);
    gtk_box_pack_start(GTK_BOX(bar), new_listener_button, FALSE, FALSE, 0);

    backend->status_label = gtk_label_new("No listeners yet");
    g_object_set_data(G_OBJECT(backend->status_label), "toolbox-listener-status-label", backend->status_label);
    gtk_box_pack_start(GTK_BOX(bar), backend->status_label, FALSE, FALSE, 0);

    GtkWidget *sidebar_toggle = gtk_toggle_button_new_with_label("Sidebar");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sidebar_toggle), TRUE);
    g_signal_connect(sidebar_toggle, "toggled", G_CALLBACK(on_toggle_panel), sidebar);
    gtk_box_pack_start(GTK_BOX(bar), sidebar_toggle, FALSE, FALSE, 0);

    GtkWidget *bottom_toggle = gtk_toggle_button_new_with_label("Bottom Panel");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bottom_toggle), TRUE);
    g_signal_connect(bottom_toggle, "toggled", G_CALLBACK(on_toggle_panel), bottom_panel);
    gtk_box_pack_start(GTK_BOX(bar), bottom_toggle, FALSE, FALSE, 0);

    return bar;
}

static GtkWidget *build_workbench_layout(GtkBackend *backend) {
    Workspace *workspace = workbench_get_workspace(backend->workbench);

    GtkWidget *sidebar = build_sidebar();
    GtkWidget *bottom_panel = build_bottom_panel(backend);
    GtkWidget *top_bar = build_top_bar(backend, sidebar, bottom_panel);

    backend->notebook = gtk_notebook_new();
    g_signal_connect(backend->notebook, "switch-page", G_CALLBACK(on_notebook_switch_page), workspace);

    GtkWidget *add_button = gtk_button_new_with_label("+ Terminal");
    gtk_button_set_relief(GTK_BUTTON(add_button), GTK_RELIEF_NONE);
    g_signal_connect(add_button, "clicked", G_CALLBACK(on_add_tab_clicked), backend);
    gtk_widget_show(add_button);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(backend->notebook), add_button, GTK_PACK_END);

    backend->next_terminal_number = 1;
    Tab *first_tab = tab_create(TAB_TYPE_TERMINAL, "Terminal 1");
    workspace_add_tab(workspace, first_tab);
    add_tab_page(backend, first_tab, TRUE);
    backend->next_terminal_number = 2;

    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(hpaned), sidebar, FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(hpaned), backend->notebook, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(hpaned), 220);

    GtkWidget *vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(vpaned), hpaned, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(vpaned), bottom_panel, FALSE, FALSE);
    gtk_paned_set_position(GTK_PANED(vpaned), 480);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(root), top_bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), vpaned, TRUE, TRUE, 0);

    return root;
}

/* gtk_window_close()/destroy can free the window's children (including
 * status_label, notebook, and everything the notebook owns) before the
 * main loop actually notices the last window is gone and returns from
 * g_application_run - the 100ms tick can fire in that gap. Null every
 * such pointer out here so on_tick's own guard (see there) can bail out
 * before touching freed memory, instead of relying on each individual
 * refresh function to separately guard against it. */
static void on_window_destroy(GtkWidget *window, gpointer user_data) {
    (void)window;
    GtkBackend *backend = user_data;
    backend->status_label = NULL;
    backend->notebook = NULL;
    backend->object_panel_store = NULL;
}

static void on_activate(GtkApplication *gtk_app, gpointer user_data) {
    GtkBackend *backend = user_data;

    GtkWidget *window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(window), "toolbox");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 700);
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);

    gtk_container_add(GTK_CONTAINER(window), build_workbench_layout(backend));
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), backend);

    gtk_widget_show_all(window);

    /* The GUI's per-tick event pump - see on_tick's comment. 100ms is
     * plenty responsive for a human watching a status label and cheap
     * enough to run for the app's whole lifetime. */
    backend->tick_source_id = g_timeout_add(100, on_tick, backend);
}

void *platform_ui_create(Workbench *workbench) {
    GtkBackend *backend = malloc(sizeof(GtkBackend));
    backend->workbench = workbench;
    backend->notebook = NULL;
    backend->next_terminal_number = 1;
    backend->terminal_entries = g_ptr_array_new();
    backend->listener_system = workbench_get_listener_system(workbench);
    backend->last_listener_id = 0;
    backend->status_label = NULL;
    backend->next_listener_number = 1;
    backend->tick_source_id = 0;
    backend->gtk_app = gtk_application_new("dev.toolbox.app", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(backend->gtk_app, "activate", G_CALLBACK(on_activate), backend);
    return backend;
}

int platform_ui_run(void *backend_ptr, int argc, char **argv) {
    GtkBackend *backend = backend_ptr;
    return g_application_run(G_APPLICATION(backend->gtk_app), argc, argv);
}

void platform_ui_destroy(void *backend_ptr) {
    GtkBackend *backend = backend_ptr;
    if (!backend) {
        return;
    }

    /* The main loop has already exited by the time this runs (this is
     * only called after platform_ui_run() returns), so the tick can't
     * actually fire again regardless - removing the source explicitly
     * is just more honest than relying on that implicitly. */
    if (backend->tick_source_id != 0) {
        g_source_remove(backend->tick_source_id);
    }

    /* Individually-closed tabs already removed and freed their entry in
     * on_tab_close_clicked; this covers whatever's left when the whole
     * window closes without every tab being closed first. The View holds
     * its own widget reference (see terminal_create's g_object_ref_sink),
     * so it's still safe to destroy here even though the window - and
     * therefore backend->notebook - is long gone by this point. */
    for (guint i = 0; i < backend->terminal_entries->len; i++) {
        TerminalEntry *entry = g_ptr_array_index(backend->terminal_entries, i);
        terminal_destroy(entry->view);
        terminal_session_destroy(entry->session);
        g_free(entry);
    }
    g_ptr_array_free(backend->terminal_entries, TRUE);

    g_object_unref(backend->gtk_app);
    free(backend);
}
