#include "ui_gtk_backend.h"
#include "ui_gtk_tabs_internal.h"
#include "ui_gtk_editor_internal.h"

/* --- Tab close/quit confirmation --------------------------------------
 * Generalizes Phase 10's single guard (confirm before closing a
 * *running* listener tab) into a sequential batch that can run over
 * any list of pages - the × button and Close Others/Close All
 * (ui_gtk_tab_labels.c) and quitting the whole application
 * (prepare_window_close) all share this same per-page decision and the
 * same two dialogs, so a running listener or a modified editor tab is
 * guarded identically no matter which path triggered the close. A
 * Cancel response at any point stops the whole batch where it is -
 * whatever already closed stays closed, the rest (and the window, for a
 * quit) is left exactly as it was. */
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

void run_close_operation(GtkBackend *backend, GPtrArray *pages,
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
/* --- end tab close/quit confirmation ----------------------------------- */
