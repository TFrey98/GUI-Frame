#ifndef WORKBENCH_TEST_GTK_UTILS_H
#define WORKBENCH_TEST_GTK_UTILS_H

#include <gtk/gtk.h>

/* Recursively collects every descendant of `widget` (inclusive) whose type
 * matches `type` into `out`. Lets smoke tests locate widgets built by the
 * platform backend without needing test-only accessors on it. */
static inline void collect_by_type(GtkWidget *widget, GPtrArray *out, GType type) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type)) {
        g_ptr_array_add(out, widget);
    }
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *l = children; l; l = l->next) {
            collect_by_type(GTK_WIDGET(l->data), out, type);
        }
        g_list_free(children);
    }
}

/*
 * Answers the two tab-close confirmations on a test's behalf.
 *
 * "This listener is running" and "Save changes to X?" are raised only when
 * there is something to confirm, so they appear at whatever moment a test
 * happens to close a tab holding a running listener or a modified
 * document - typically its own teardown - rather than at a fixed step. No
 * test step is waiting for them there, the app will not finish closing
 * until something responds, and the run hangs until a human clicks. That
 * is what made these tests unrunnable unattended.
 *
 * The answers are the ones an unattended run wants: stop the listener, so
 * its port is released before the next test binds the same one, and
 * discard unsaved changes, so teardown writes nothing to disk.
 *
 * Not for tests that exercise these dialogs deliberately - see
 * editor_close_confirmation_smoke, which drives them itself.
 */
static inline gboolean test_answer_close_confirmations(gpointer user_data) {
    (void)user_data;
    GList *toplevels = gtk_window_list_toplevels();
    for (GList *l = toplevels; l; l = l->next) {
        if (!GTK_IS_DIALOG(l->data)) {
            continue;
        }
        if (g_object_get_data(G_OBJECT(l->data), "workbench-listener-close-dialog")) {
            gtk_dialog_response(GTK_DIALOG(l->data), GTK_RESPONSE_YES); /* Stop Listener */
        } else if (g_object_get_data(G_OBJECT(l->data), "workbench-editor-close-dialog")) {
            gtk_dialog_response(GTK_DIALOG(l->data), GTK_RESPONSE_NO); /* Discard */
        }
    }
    g_list_free(toplevels);
    return G_SOURCE_CONTINUE;
}

/* Call from main() before app_run(). */
static inline void install_close_confirmation_answers(void) {
    g_timeout_add(25, test_answer_close_confirmations, NULL);
}

#endif /* WORKBENCH_TEST_GTK_UTILS_H */
