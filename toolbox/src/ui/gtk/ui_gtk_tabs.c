#include "ui_gtk_internal.h"

/* Shared by a tab label's click and entry-commit handlers so both can flip
 * between the display label and the editable rename entry for the same
 * tab. Owned by the event_box in build_tab_label via g_object_set_data_full,
 * so it's freed automatically when that widget is destroyed. */
typedef struct TabLabelData {
    Tab *tab;
    GtkWidget *label;
    GtkWidget *entry;
    GtkWidget *page; /* back-reference, needed by the right-click context menu below */
} TabLabelData;

/* Forward declared for on_label_button_press/build_tab_label below -
 * defined further down, after the close/quit confirmation section (it
 * needs run_close_operation). */
static void popup_tab_context_menu(GtkWidget *page, GdkEventButton *event);
static gboolean on_tab_label_popup_menu(GtkWidget *event_box, gpointer user_data);

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
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
        popup_tab_context_menu(data->page, event);
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
    } else if (tab->type == TAB_TYPE_EDITOR || tab->type == TAB_TYPE_BINARY_INFO) {
        editor_document_destroy((EditorDocument *)tab->backend_data);
        tab->backend_data = NULL;
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
    g_object_set_data(G_OBJECT(page), "toolbox-tab-label-widget", label);

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
    data->page = page;
    g_object_set_data_full(G_OBJECT(event_box), "toolbox-label-data", data, g_free);

    g_signal_connect(event_box, "button-press-event", G_CALLBACK(on_label_button_press), data);
    g_signal_connect(event_box, "popup-menu", G_CALLBACK(on_tab_label_popup_menu), data);
    g_signal_connect(entry, "activate", G_CALLBACK(commit_rename), data);
    g_signal_connect(entry, "focus-out-event", G_CALLBACK(on_entry_focus_out), data);

    GtkWidget *close_button = gtk_button_new_with_label("×");
    gtk_button_set_relief(GTK_BUTTON(close_button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close_button, FALSE);
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_tab_close_clicked), page);
    g_object_set_data(G_OBJECT(page), "toolbox-tab-close-button", close_button);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(box), event_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), close_button, FALSE, FALSE, 0);
    gtk_widget_show_all(box);

    return box;
}

void update_tab_label_text(GtkWidget *page) {
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    GtkWidget *label = g_object_get_data(G_OBJECT(page), "toolbox-tab-label-widget");
    if (!tab || !label) {
        return;
    }
    gboolean modified = tab->type == TAB_TYPE_EDITOR && ((EditorDocument *)tab->backend_data)->modified;
    if (modified) {
        gchar *text = g_strdup_printf("%s \xE2\x97\x8F", tab->title);
        gtk_label_set_text(GTK_LABEL(label), text);
        g_free(text);
    } else {
        gtk_label_set_text(GTK_LABEL(label), tab->title);
    }
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

/* --- end Listener console tab ----------------------------------------- */

/* --- Tab close/quit confirmation --------------------------------------
 * Generalizes Phase 10's single guard (confirm before closing a
 * *running* listener tab) into a sequential batch that can run over
 * any list of pages - the × button, Close Others/Close All (the tab
 * label's own right-click menu, further below), and quitting the whole
 * application (prepare_window_close) all share this same per-page
 * decision and the same two dialogs, so a running listener or a
 * modified editor tab is guarded identically no matter which path
 * triggered the close. A Cancel response at any point stops the whole
 * batch where it is - whatever already closed stays closed, the rest
 * (and the window, for a quit) is left exactly as it was. */
typedef struct CloseOperation {
    GtkBackend *backend;
    GPtrArray *pages; /* GtkWidget* still to process - owned by this struct */
    guint index;
    void (*on_finished)(GtkBackend *backend, gboolean all_completed, gpointer user_data);
    gpointer user_data;
} CloseOperation;

static void close_operation_advance(CloseOperation *op);

static void close_operation_finish(CloseOperation *op, gboolean all_completed) {
    if (op->on_finished) {
        op->on_finished(op->backend, all_completed, op->user_data);
    }
    g_ptr_array_free(op->pages, TRUE);
    g_free(op);
}

static void close_operation_continue(CloseOperation *op) {
    op->index++;
    close_operation_advance(op);
}

static void on_close_operation_listener_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    CloseOperation *op = user_data;
    GtkWidget *page = g_ptr_array_index(op->pages, op->index);
    gtk_widget_destroy(GTK_WIDGET(dialog));

    if (response_id == GTK_RESPONSE_YES) { /* "Stop Listener" */
        ListenerPageContext *ctx = g_object_get_data(G_OBJECT(page), "toolbox-listener-page-context");
        listener_manager_stop(ctx->backend->listener_system->listener_manager, ctx->listener_id);
        close_tab_page(page);
        close_operation_continue(op);
    } else if (response_id == GTK_RESPONSE_ACCEPT) { /* "Close Tab" */
        close_tab_page(page);
        close_operation_continue(op);
    } else {
        /* Cancel or dismissing the dialog: the whole batch stops here -
         * this tab (and anything after it) stays exactly as it was. */
        close_operation_finish(op, FALSE);
    }
}

static void on_close_operation_editor_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    CloseOperation *op = user_data;
    GtkWidget *page = g_ptr_array_index(op->pages, op->index);
    gtk_widget_destroy(GTK_WIDGET(dialog));

    if (response_id == GTK_RESPONSE_YES) { /* "Save" */
        EditorSaveResult result = save_editor_page(op->backend, page);
        if (result != EDITOR_SAVE_OK) {
            show_explorer_error(op->backend, editor_save_error_message(result));
            /* A failed save must never lose data - stop the whole batch
             * here, leaving this (and every remaining) tab open. */
            close_operation_finish(op, FALSE);
            return;
        }
        close_tab_page(page);
        close_operation_continue(op);
    } else if (response_id == GTK_RESPONSE_NO) { /* "Discard" */
        close_tab_page(page);
        close_operation_continue(op);
    } else {
        close_operation_finish(op, FALSE); /* Cancel */
    }
}

static void close_operation_advance(CloseOperation *op) {
    if (op->index >= op->pages->len) {
        close_operation_finish(op, TRUE);
        return;
    }

    GtkWidget *page = g_ptr_array_index(op->pages, op->index);
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");

    if (tab->type == TAB_TYPE_LISTENER) {
        ListenerPageContext *ctx = g_object_get_data(G_OBJECT(page), "toolbox-listener-page-context");
        const Listener *listener =
            object_registry_get_listener(ctx->backend->listener_system->registry, ctx->listener_id);
        if (listener && listener->runtime.state == LISTENER_STATE_RUNNING) {
            GtkWindow *parent = gtk_application_get_active_window(op->backend->gtk_app);
            GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                                         GTK_BUTTONS_NONE,
                                                         "This listener is running. What would you like to do?");
            gtk_dialog_add_button(GTK_DIALOG(dialog), "Close Tab", GTK_RESPONSE_ACCEPT);
            gtk_dialog_add_button(GTK_DIALOG(dialog), "Stop Listener", GTK_RESPONSE_YES);
            gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
            g_signal_connect(dialog, "response", G_CALLBACK(on_close_operation_listener_response), op);
            gtk_widget_show_all(dialog);
            return;
        }
    } else if (tab->type == TAB_TYPE_EDITOR) {
        EditorDocument *doc = tab->backend_data;
        if (doc->modified) {
            GtkWindow *parent = gtk_application_get_active_window(op->backend->gtk_app);
            GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                                         GTK_BUTTONS_NONE, "Save changes to %s?", tab->title);
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Save", GTK_RESPONSE_YES);
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Discard", GTK_RESPONSE_NO);
            gtk_dialog_add_button(GTK_DIALOG(dialog), "_Cancel", GTK_RESPONSE_CANCEL);
            g_signal_connect(dialog, "response", G_CALLBACK(on_close_operation_editor_response), op);
            gtk_widget_show_all(dialog);
            return;
        }
    }

    /* Neither guard applies - close outright and move on. */
    close_tab_page(page);
    close_operation_continue(op);
}

static void run_close_operation(GtkBackend *backend, GPtrArray *pages,
                                 void (*on_finished)(GtkBackend *, gboolean, gpointer), gpointer user_data) {
    CloseOperation *op = g_new(CloseOperation, 1);
    op->backend = backend;
    op->pages = pages;
    op->index = 0;
    op->on_finished = on_finished;
    op->user_data = user_data;
    close_operation_advance(op);
}

void close_page_with_confirmation(GtkBackend *backend, GtkWidget *page) {
    GPtrArray *pages = g_ptr_array_new();
    g_ptr_array_add(pages, page);
    run_close_operation(backend, pages, NULL, NULL);
}

static void on_prepare_window_close_finished(GtkBackend *backend, gboolean all_completed, gpointer user_data) {
    (void)backend;
    GtkWindow *window = user_data;
    if (all_completed) {
        gtk_widget_destroy(GTK_WIDGET(window));
    }
    /* !all_completed: the user canceled partway through - the window
     * and every tab that hadn't been reached yet are left exactly as
     * they were, matching a single close's own Cancel behavior. */
}

void prepare_window_close(GtkBackend *backend, GtkWindow *window) {
    GPtrArray *pages = g_ptr_array_new();
    if (backend->notebook) {
        int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
        for (int i = 0; i < n; i++) {
            g_ptr_array_add(pages, gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i));
        }
    }
    run_close_operation(backend, pages, on_prepare_window_close_finished, window);
}

/* Bound to the × button on every tab label - derives backend from the
 * page itself (add_tab_page tags "toolbox-backend" on every page it
 * builds) since the button's own "clicked" signal only ever carries
 * the page as user_data. */
static void on_tab_close_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWidget *page = GTK_WIDGET(user_data);
    GtkBackend *backend = g_object_get_data(G_OBJECT(page), "toolbox-backend");
    close_page_with_confirmation(backend, page);
}

/* --- Tab label right-click menu: Close / Close Others / Close All ----- */

static void on_tab_context_close(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *page = user_data;
    GtkBackend *backend = g_object_get_data(G_OBJECT(page), "toolbox-backend");
    close_page_with_confirmation(backend, page);
}

static void on_tab_context_close_others(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *page = user_data;
    GtkBackend *backend = g_object_get_data(G_OBJECT(page), "toolbox-backend");
    GPtrArray *pages = g_ptr_array_new();
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *candidate = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i);
        if (candidate != page) {
            g_ptr_array_add(pages, candidate);
        }
    }
    run_close_operation(backend, pages, NULL, NULL);
}

static void on_tab_context_close_all(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *page = user_data;
    GtkBackend *backend = g_object_get_data(G_OBJECT(page), "toolbox-backend");
    GPtrArray *pages = g_ptr_array_new();
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        g_ptr_array_add(pages, gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i));
    }
    run_close_operation(backend, pages, NULL, NULL);
}

/* event NULL means the keyboard "popup-menu" signal (see
 * on_tab_label_popup_menu below) - gtk_menu_popup_at_pointer() already
 * accepts NULL there, same convention popup_object_context_menu/
 * popup_explorer_context_menu already established. Tags the built menu
 * on page itself so it stays discoverable after this call returns -
 * tests read the menu this way, a real user never needs to. */
static void popup_tab_context_menu(GtkWidget *page, GdkEventButton *event) {
    GtkWidget *menu = gtk_menu_new();

    GtkWidget *close_item = gtk_menu_item_new_with_label("Close");
    GtkWidget *close_others_item = gtk_menu_item_new_with_label("Close Others");
    GtkWidget *close_all_item = gtk_menu_item_new_with_label("Close All");
    g_signal_connect(close_item, "activate", G_CALLBACK(on_tab_context_close), page);
    g_signal_connect(close_others_item, "activate", G_CALLBACK(on_tab_context_close_others), page);
    g_signal_connect(close_all_item, "activate", G_CALLBACK(on_tab_context_close_all), page);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), close_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), close_others_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), close_all_item);

    g_object_set_data(G_OBJECT(page), "toolbox-tab-context-menu", menu);
    gtk_widget_show_all(menu);
    if (event) {
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    } else {
        gtk_menu_popup_at_widget(GTK_MENU(menu), page, GDK_GRAVITY_CENTER, GDK_GRAVITY_CENTER, NULL);
    }
}

/* Shift+F10/Menu-key equivalent of the right-click handler above - same
 * "real, standard GTK signal" convention this suite already relies on
 * for the object panel's and explorer's own context menus. */
static gboolean on_tab_label_popup_menu(GtkWidget *event_box, gpointer user_data) {
    (void)event_box;
    TabLabelData *data = user_data;
    popup_tab_context_menu(data->page, NULL);
    return TRUE;
}
/* --- end tab close/quit confirmation ----------------------------------- */

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
void focus_page(GtkBackend *backend, GtkWidget *page) {
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

void add_tab_page(GtkBackend *backend, Tab *tab, gboolean focus) {
    Workspace *workspace = workbench_get_workspace(backend->workbench);

    GtkWidget *page;
    if (tab->type == TAB_TYPE_TERMINAL) {
        page = build_terminal_page(backend, tab);
    } else if (tab->type == TAB_TYPE_LISTENER) {
        page = build_listener_page(backend, tab);
    } else if (tab->type == TAB_TYPE_CONNECTION_TERMINAL) {
        page = build_connection_terminal_page(backend, tab);
    } else if (tab->type == TAB_TYPE_EDITOR) {
        page = build_editor_page(backend, tab);
    } else if (tab->type == TAB_TYPE_BINARY_INFO) {
        page = build_binary_info_page(backend, tab);
    } else {
        page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_set_border_width(GTK_CONTAINER(page), 12);
        gtk_box_pack_start(GTK_BOX(page), gtk_label_new(tab->title), FALSE, FALSE, 0);
    }

    g_object_set_data(G_OBJECT(page), "toolbox-tab", tab);
    g_object_set_data(G_OBJECT(page), "toolbox-workspace", workspace);
    g_object_set_data(G_OBJECT(page), "toolbox-backend", backend);

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
        Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
        if (tab && tab->type == TAB_TYPE_LISTENER) {
            refresh_listener_page(backend, page, listener_tab_id(tab));
        }
    }
}

void on_add_tab_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkBackend *backend = user_data;
    Workspace *workspace = workbench_get_workspace(backend->workbench);

    char title[32];
    g_snprintf(title, sizeof(title), "Terminal %d", backend->next_terminal_number++);

    Tab *tab = tab_create(TAB_TYPE_TERMINAL, title);
    workspace_add_tab(workspace, tab);
    add_tab_page(backend, tab, TRUE);
}

void on_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
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
