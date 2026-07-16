/*
 * Repeatedly creates, runs, and destroys an App to exercise the Milestone 1
 * definition of done: the application must open and close its window
 * across many cycles without crashing.
 */
#include <gtk/gtk.h>
#include <stdio.h>

#include "app/app.h"

#define ITERATIONS 5

static gboolean auto_close_window(gpointer user_data) {
    (void)user_data;

    GApplication *default_app = g_application_get_default();
    if (default_app) {
        GList *windows = gtk_application_get_windows(GTK_APPLICATION(default_app));
        if (windows) {
            gtk_window_close(GTK_WINDOW(windows->data));
        }
    }
    return G_SOURCE_REMOVE;
}

int main(void) {
    for (int i = 0; i < ITERATIONS; i++) {
        App *app = app_create(0, NULL);
        g_timeout_add(150, auto_close_window, NULL);

        int status = app_run(app);
        app_destroy(app);

        if (status != 0) {
            fprintf(stderr, "lifecycle_smoke: iteration %d exited with status %d\n", i, status);
            return 1;
        }
        g_print("lifecycle_smoke: iteration %d ok\n", i);
    }

    g_print("lifecycle_smoke: %d iterations completed cleanly\n", ITERATIONS);
    return 0;
}
