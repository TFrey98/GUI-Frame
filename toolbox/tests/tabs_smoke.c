/*
 * Exercises Milestone 3's definition of done: dummy tabs can be created,
 * selected, renamed, and closed through the real GtkNotebook UI. GLib
 * criticals are promoted to fatal so a broken widget tree aborts the test
 * instead of just logging a warning.
 */
#include <gtk/gtk.h>
#include <stdio.h>

#include "app/app.h"
#include "test_gtk_utils.h"

static gboolean g_test_passed = FALSE;

static GtkWidget *find_notebook(GtkWidget *root) {
    GPtrArray *found = g_ptr_array_new();
    collect_by_type(root, found, GTK_TYPE_NOTEBOOK);
    GtkWidget *notebook = found->len > 0 ? GTK_WIDGET(g_ptr_array_index(found, 0)) : NULL;
    g_ptr_array_free(found, TRUE);
    return notebook;
}

static gboolean exercise_tabs(gpointer user_data) {
    (void)user_data;

    GApplication *default_app = g_application_get_default();
    GList *windows = gtk_application_get_windows(GTK_APPLICATION(default_app));
    GtkWindow *window = GTK_WINDOW(windows->data);

    GtkWidget *notebook = find_notebook(GTK_WIDGET(window));
    if (!notebook) {
        fprintf(stderr, "tabs_smoke: no notebook found\n");
        gtk_window_close(window);
        return G_SOURCE_REMOVE;
    }

    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) != 1) {
        fprintf(stderr, "tabs_smoke: expected 1 seeded tab, got %d\n",
                gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)));
        gtk_window_close(window);
        return G_SOURCE_REMOVE;
    }

    /* create */
    GtkWidget *add_button = gtk_notebook_get_action_widget(GTK_NOTEBOOK(notebook), GTK_PACK_END);
    gtk_button_clicked(GTK_BUTTON(add_button));
    gtk_button_clicked(GTK_BUTTON(add_button));

    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) != 3) {
        fprintf(stderr, "tabs_smoke: expected 3 tabs after creating 2 more, got %d\n",
                gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)));
        gtk_window_close(window);
        return G_SOURCE_REMOVE;
    }

    /* select */
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 0);

    GtkWidget *page0 = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), 0);
    GtkWidget *tab_label = gtk_notebook_get_tab_label(GTK_NOTEBOOK(notebook), page0);

    /* Scope the search to the event box (title + rename entry), not the
     * whole tab strip - the close button has its own internal GtkLabel
     * for its "x" glyph that would otherwise be picked up too. */
    GPtrArray *event_boxes = g_ptr_array_new();
    collect_by_type(tab_label, event_boxes, GTK_TYPE_EVENT_BOX);
    if (event_boxes->len != 1) {
        fprintf(stderr, "tabs_smoke: expected one event box in the tab, got %u\n", event_boxes->len);
        gtk_window_close(window);
        return G_SOURCE_REMOVE;
    }
    GtkWidget *title_area = GTK_WIDGET(g_ptr_array_index(event_boxes, 0));
    g_ptr_array_free(event_boxes, TRUE);

    GPtrArray *entries = g_ptr_array_new();
    collect_by_type(title_area, entries, GTK_TYPE_ENTRY);
    GPtrArray *labels = g_ptr_array_new();
    collect_by_type(title_area, labels, GTK_TYPE_LABEL);

    if (entries->len != 1 || labels->len != 1) {
        fprintf(stderr, "tabs_smoke: expected one entry and one label in the tab, got %u/%u\n",
                entries->len, labels->len);
        gtk_window_close(window);
        return G_SOURCE_REMOVE;
    }

    /* rename */
    GtkWidget *entry = GTK_WIDGET(g_ptr_array_index(entries, 0));
    GtkWidget *label = GTK_WIDGET(g_ptr_array_index(labels, 0));

    gtk_widget_show(entry);
    gtk_entry_set_text(GTK_ENTRY(entry), "Renamed");
    g_signal_emit_by_name(entry, "activate");

    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), "Renamed") != 0) {
        fprintf(stderr, "tabs_smoke: rename did not update the tab label\n");
        gtk_window_close(window);
        return G_SOURCE_REMOVE;
    }

    /* close */
    GPtrArray *buttons = g_ptr_array_new();
    collect_by_type(tab_label, buttons, GTK_TYPE_BUTTON);
    if (buttons->len != 1) {
        fprintf(stderr, "tabs_smoke: expected one close button, got %u\n", buttons->len);
        gtk_window_close(window);
        return G_SOURCE_REMOVE;
    }
    gtk_button_clicked(GTK_BUTTON(g_ptr_array_index(buttons, 0)));

    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) != 2) {
        fprintf(stderr, "tabs_smoke: expected 2 tabs after closing one, got %d\n",
                gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)));
        gtk_window_close(window);
        return G_SOURCE_REMOVE;
    }

    g_ptr_array_free(entries, TRUE);
    g_ptr_array_free(labels, TRUE);
    g_ptr_array_free(buttons, TRUE);

    g_test_passed = TRUE;
    gtk_window_close(window);
    return G_SOURCE_REMOVE;
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    App *app = app_create(0, NULL);
    g_timeout_add(200, exercise_tabs, NULL);

    int status = app_run(app);
    app_destroy(app);

    if (status != 0) {
        fprintf(stderr, "tabs_smoke: window run exited with status %d\n", status);
        return 1;
    }
    if (!g_test_passed) {
        fprintf(stderr, "tabs_smoke: FAILED\n");
        return 1;
    }

    g_print("tabs_smoke: create/select/rename/close all verified\n");
    return 0;
}
