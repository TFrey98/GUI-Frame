/*
 * The pane divider must not be able to squeeze a terminal narrower than a
 * shell prompt.
 *
 * VTE asks for only a couple of columns of its own, so with nothing else
 * constraining it the divider could take the terminal down to 26 columns.
 * A 27-character prompt does not fit in that, and readline responds by
 * truncating it (rendering "<client-14e9:~$") and re-laying out the line,
 * which reads as the terminal spontaneously adding one.
 *
 * MIN_TERMINAL_COLUMNS puts a floor under the terminal page, so the
 * divider runs out of travel while the terminal is still usable. This
 * checks the floor actually binds rather than that any particular prompt
 * survives, which would depend on whoever's PS1 is in the environment.
 */
#include <gtk/gtk.h>
#include <vte/vte.h>
#include <stdio.h>

#include "app/app.h"
#include "test_gtk_utils.h"
#include "ui/gtk/ui_gtk_backend.h"

#define TICK_MS 50
#define SETTLE_TICKS 12
#define SETTLE_AFTER_DRAG_TICKS 6
/* Deliberately a number of its own rather than MIN_TERMINAL_COLUMNS.
 * Asserting against the production constant would make this test move
 * whenever that constant moves - lowering the floor would lower the
 * expectation with it and the test would keep passing, which is exactly
 * what happened when it was first written. This is the independent claim:
 * a terminal narrower than about thirty columns cannot hold an ordinary
 * shell prompt, whatever the production constant happens to say. */
#define MIN_USABLE_COLUMNS 30

typedef struct TestState {
    int tick;
    gboolean dragged;
    int settle;
    gboolean failed;
    gboolean done;
} TestState;

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "terminal_minimum_width_smoke: %s\n", msg);
    test->failed = TRUE;
    test->done = TRUE;
    GList *windows = gtk_window_list_toplevels();
    for (GList *l = windows; l; l = l->next) {
        gtk_window_close(GTK_WINDOW(l->data));
    }
    g_list_free(windows);
}

static GtkWidget *find_hpaned(GtkWidget *window) {
    GPtrArray *paneds = g_ptr_array_new();
    collect_by_type(window, paneds, GTK_TYPE_PANED);
    GtkWidget *found = NULL;
    for (guint i = 0; i < paneds->len; i++) {
        GtkWidget *candidate = g_ptr_array_index(paneds, i);
        if (gtk_orientable_get_orientation(GTK_ORIENTABLE(candidate)) == GTK_ORIENTATION_HORIZONTAL) {
            found = candidate;
            break;
        }
    }
    g_ptr_array_free(paneds, TRUE);
    return found;
}

static gboolean drive(gpointer data) {
    TestState *test = data;
    test->tick++;

    GList *windows = gtk_window_list_toplevels();
    GtkWidget *window = windows && windows->data ? GTK_WIDGET(windows->data) : NULL;
    g_list_free(windows);
    if (!window || !gtk_widget_get_realized(window)) {
        return G_SOURCE_CONTINUE;
    }
    if (test->tick < SETTLE_TICKS) {
        return G_SOURCE_CONTINUE;
    }

    GPtrArray *vtes = g_ptr_array_new();
    collect_by_type(window, vtes, VTE_TYPE_TERMINAL);
    GtkWidget *vte = vtes->len ? g_ptr_array_index(vtes, 0) : NULL;
    g_ptr_array_free(vtes, TRUE);
    GtkWidget *paned = find_hpaned(window);
    if (!vte || !paned) {
        fail(test, "could not find the terminal widget or the horizontal paned");
        return G_SOURCE_REMOVE;
    }

    /* Ask for far more than the divider can give, so it comes to rest at
     * the end of its travel - the position a user reaches by dragging the
     * explorer edge as far as it will go. */
    if (!test->dragged) {
        gtk_paned_set_position(GTK_PANED(paned), 100000);
        gtk_container_check_resize(GTK_CONTAINER(paned));
        test->dragged = TRUE;
        return G_SOURCE_CONTINUE;
    }
    if (test->settle++ < SETTLE_AFTER_DRAG_TICKS) {
        return G_SOURCE_CONTINUE;
    }

    glong columns = vte_terminal_get_column_count(VTE_TERMINAL(vte));
    if (columns < MIN_USABLE_COLUMNS) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "the divider squeezed the terminal to %ld columns, under the %d needed to hold an "
                 "ordinary shell prompt on one line (MIN_TERMINAL_COLUMNS is %d)",
                 columns, MIN_USABLE_COLUMNS, MIN_TERMINAL_COLUMNS);
        fail(test, msg);
        return G_SOURCE_REMOVE;
    }

    test->done = TRUE;
    gtk_window_close(GTK_WINDOW(window));
    return G_SOURCE_REMOVE;
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "terminal_minimum_width_smoke: app_create failed\n");
        return 1;
    }

    install_close_confirmation_answers();
    g_timeout_add(TICK_MS, drive, &test);

    int status = app_run(app);
    app_destroy(app);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "terminal_minimum_width_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "terminal_minimum_width_smoke: test did not complete\n");
        return 1;
    }

    g_print("terminal_minimum_width_smoke: the divider runs out of travel while the terminal is "
            "still wider than a shell prompt\n");
    return 0;
}
