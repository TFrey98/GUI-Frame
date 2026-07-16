/*
 * Exercises Milestone 2's definition of done: resizing the window and
 * dragging every pane divider through several positions, plus toggling
 * the sidebar/bottom-panel visibility, must not corrupt the layout.
 * GLib criticals are promoted to fatal so a broken widget tree aborts
 * the test instead of just logging a warning.
 */
#include <gtk/gtk.h>
#include <stdio.h>

#include "app/app.h"
#include "test_gtk_utils.h"

#define ITERATIONS 3

static gboolean exercise_layout(gpointer user_data) {
    (void)user_data;

    GApplication *default_app = g_application_get_default();
    if (!default_app) {
        return G_SOURCE_REMOVE;
    }
    GList *windows = gtk_application_get_windows(GTK_APPLICATION(default_app));
    if (!windows) {
        return G_SOURCE_REMOVE;
    }
    GtkWindow *window = GTK_WINDOW(windows->data);

    gtk_window_resize(window, 1400, 900);
    gtk_window_resize(window, 800, 500);
    gtk_window_resize(window, 1100, 700);

    GPtrArray *paneds = g_ptr_array_new();
    collect_by_type(GTK_WIDGET(window), paneds, GTK_TYPE_PANED);
    for (guint i = 0; i < paneds->len; i++) {
        GtkPaned *paned = GTK_PANED(g_ptr_array_index(paneds, i));
        gtk_paned_set_position(paned, 20);
        gtk_paned_set_position(paned, 600);
        gtk_paned_set_position(paned, 300);
    }
    g_ptr_array_free(paneds, TRUE);

    GPtrArray *toggles = g_ptr_array_new();
    collect_by_type(GTK_WIDGET(window), toggles, GTK_TYPE_TOGGLE_BUTTON);
    for (guint i = 0; i < toggles->len; i++) {
        GtkButton *button = GTK_BUTTON(g_ptr_array_index(toggles, i));
        gtk_button_clicked(button); /* hide */
        gtk_button_clicked(button); /* show again */
    }
    g_ptr_array_free(toggles, TRUE);

    gtk_window_close(window);
    return G_SOURCE_REMOVE;
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    for (int i = 0; i < ITERATIONS; i++) {
        App *app = app_create(0, NULL);
        g_timeout_add(200, exercise_layout, NULL);

        int status = app_run(app);
        app_destroy(app);

        if (status != 0) {
            fprintf(stderr, "layout_smoke: iteration %d exited with status %d\n", i, status);
            return 1;
        }
        g_print("layout_smoke: iteration %d ok\n", i);
    }

    g_print("layout_smoke: %d iterations completed cleanly\n", ITERATIONS);
    return 0;
}
