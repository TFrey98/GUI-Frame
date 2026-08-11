#include "ui_gtk_backend.h"
#include "ui_gtk_tabs_internal.h"
#include "ui_gtk_explorer_internal.h"
#include "ui_gtk_editor_internal.h"
#include "ui_gtk_terminal_internal.h"

#include <stdio.h>
#include <string.h>

#define WATCH_DRAIN_MAX 64

/* Drains up to WATCH_DRAIN_MAX pending events from watcher and applies
 * each, deduping by new_relative_path within this one batch (keeping
 * only the last event for a given path) - the doc's "a single save may
 * cause several filesystem notifications," a second debounce layer
 * beyond IN_CLOSE_WRITE alone. */
static void drain_and_apply_watcher(GtkBackend *backend, FileWatcher *watcher, int source) {
    if (!watcher) {
        return;
    }
    FileWatchEvent events[WATCH_DRAIN_MAX];
    int count = 0;
    FileWatchEvent event;
    while (count < WATCH_DRAIN_MAX && file_watcher_try_pop_event(watcher, &event)) {
        events[count++] = event;
    }
    for (int i = 0; i < count; i++) {
        gboolean superseded = FALSE;
        for (int j = i + 1; j < count; j++) {
            if (strcmp(events[i].new_relative_path, events[j].new_relative_path) == 0) {
                superseded = TRUE;
                break;
            }
        }
        if (!superseded) {
            apply_file_watch_event(backend, source, &events[i]);
        }
    }
}

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

/* g_timeout_add callback: the GUI's per-tick event pump. Never does
 * socket I/O itself - it only reads registry state and drives managers;
 * worker threads remain responsible for blocking network operations. */
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
    drain_and_apply_watcher(backend, backend->file_watcher, EXPLORER_SOURCE_FILES);
    drain_and_apply_watcher(backend, backend->toolkit_watcher, EXPLORER_SOURCE_TOOLKIT);

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

static void on_save_all_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    save_all_modified_editors((GtkBackend *)user_data);
}

static void on_dark_mode_toggled(GtkToggleButton *button, gpointer user_data) {
    apply_dark_mode((GtkBackend *)user_data, gtk_toggle_button_get_active(button));
}

static void on_search_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    open_or_present_search_window((GtkBackend *)user_data);
}

static GtkWidget *build_top_bar(GtkBackend *backend, GtkWidget *sidebar, GtkWidget *bottom_panel) {
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 6);

    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("toolbox"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new(NULL), TRUE, TRUE, 0);

    GtkWidget *save_all_button = gtk_button_new_with_label("Save All");
    g_object_set_data(G_OBJECT(save_all_button), "toolbox-save-all-button", save_all_button);
    g_signal_connect(save_all_button, "clicked", G_CALLBACK(on_save_all_clicked), backend);
    gtk_box_pack_start(GTK_BOX(bar), save_all_button, FALSE, FALSE, 0);

    GtkWidget *new_listener_button = gtk_button_new_with_label("+ New Listener");
    g_object_set_data(G_OBJECT(new_listener_button), "toolbox-new-listener-button", new_listener_button);
    g_signal_connect(new_listener_button, "clicked", G_CALLBACK(on_new_listener_clicked), backend);
    gtk_box_pack_start(GTK_BOX(bar), new_listener_button, FALSE, FALSE, 0);

    GtkWidget *search_button = gtk_button_new_with_label("\xF0\x9F\x94\x8D Search");
    g_object_set_data(G_OBJECT(search_button), "toolbox-search-open-button", search_button);
    g_signal_connect(search_button, "clicked", G_CALLBACK(on_search_clicked), backend);
    gtk_box_pack_start(GTK_BOX(bar), search_button, FALSE, FALSE, 0);

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

    GtkWidget *dark_mode_toggle = gtk_toggle_button_new_with_label("\xF0\x9F\x8C\x99 Dark Mode");
    g_object_set_data(G_OBJECT(dark_mode_toggle), "toolbox-dark-mode-toggle", dark_mode_toggle);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dark_mode_toggle), backend->dark_mode);
    g_signal_connect(dark_mode_toggle, "toggled", G_CALLBACK(on_dark_mode_toggled), backend);
    gtk_box_pack_start(GTK_BOX(bar), dark_mode_toggle, FALSE, FALSE, 0);

    return bar;
}

static GtkWidget *build_workbench_layout(GtkBackend *backend) {
    Workspace *workspace = workbench_get_workspace(backend->workbench);

    GtkWidget *sidebar = build_explorer_sidebar(backend);
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

    /* The search window is a separate top-level (see ui_gtk_search.c) -
     * gtk_application_add_window() there only means it participates in
     * the app's own "quit once every window is gone" bookkeeping, not
     * that GTK closes it automatically alongside this one. Without this,
     * closing the main window while Search is still open would leave it
     * as an orphan keeping the process alive. Its own "destroy" handler
     * nulls backend->search_window synchronously, so nothing further is
     * needed here. */
    if (backend->search_window) {
        gtk_widget_destroy(backend->search_window);
    }
}

/* Always blocks the *immediate* default close (returns TRUE) -
 * prepare_window_close either destroys the window itself synchronously
 * when nothing needs confirming, or asynchronously once its Save/
 * Discard/Cancel dialog chain resolves. The doc's "closing the
 * application prompts for every modified document." */
static gboolean on_window_delete_event(GtkWidget *window, GdkEvent *event, gpointer user_data) {
    (void)event;
    GtkBackend *backend = user_data;
    prepare_window_close(backend, GTK_WINDOW(window));
    return TRUE;
}

/* Ctrl+S / Ctrl+Shift+S / Ctrl+Shift+W / Ctrl+Shift+A - a single
 * window-level signal handler rather than a GtkAccelGroup/GAction
 * framework, matching this project's existing plain-signal-handler
 * style (no prior accelerator/action-group usage anywhere in this
 * codebase). gdk_keyval_to_lower() normalizes the keyval so either
 * shift-state reports the shortcut correctly across keyboard layouts. */
static gboolean on_window_key_press(GtkWidget *window, GdkEventKey *event, gpointer user_data) {
    GtkBackend *backend = user_data;
    if (!(event->state & GDK_CONTROL_MASK)) {
        return FALSE;
    }
    gboolean shift = (event->state & GDK_SHIFT_MASK) != 0;
    guint lower = gdk_keyval_to_lower(event->keyval);

    if (lower == GDK_KEY_s && !shift) {
        trigger_save_active_editor(backend);
        return TRUE;
    }
    if (lower == GDK_KEY_s && shift) {
        trigger_save_as_active_editor(backend, GTK_WINDOW(window));
        return TRUE;
    }
    if (lower == GDK_KEY_w && shift) {
        gint idx = backend->notebook ? gtk_notebook_get_current_page(GTK_NOTEBOOK(backend->notebook)) : -1;
        if (idx >= 0) {
            close_page_with_confirmation(backend, gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), idx));
        }
        return TRUE;
    }
    if (lower == GDK_KEY_a && shift) {
        save_all_modified_editors(backend);
        return TRUE;
    }
    return FALSE;
}

void on_activate(GtkApplication *gtk_app, gpointer user_data) {
    GtkBackend *backend = user_data;

    gtk_theme_init(backend);

    GtkWidget *window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(window), "toolbox");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 700);
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);

    gtk_container_add(GTK_CONTAINER(window), build_workbench_layout(backend));
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), backend);
    g_signal_connect(window, "delete-event", G_CALLBACK(on_window_delete_event), backend);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_window_key_press), backend);

    gtk_widget_show_all(window);

    /* The GUI's per-tick event pump - see on_tick's comment. 100ms is
     * plenty responsive for a human watching a status label and cheap
     * enough to run for the app's whole lifetime. */
    backend->tick_source_id = g_timeout_add(100, on_tick, backend);
}
