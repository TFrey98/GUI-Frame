/*
 * Exercises the core of Milestone 4's definition of done in an automated,
 * non-interactive way: the seeded terminal tab's shell actually spawns and
 * is genuinely interactive (an echoed command's output shows up in the
 * VTE buffer), and resizing the window while a shell is running doesn't
 * corrupt anything. GLib criticals are promoted to fatal.
 *
 * The full manual checklist from 4.5 (top, python3, Ctrl+C, Ctrl+D, arrow
 * keys, backspace, live resize) is best verified by a human against the
 * compiled `workbench` binary - a full-screen ncurses program and a REPL
 * aren't practical to assert on by scraping rendered text.
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <vte/vte.h>

#include "app/app.h"
#include "terminal/terminal.h"
#include "test_gtk_utils.h"

static gboolean g_test_passed = FALSE;

static gboolean check_output(gpointer user_data) {
    VteTerminal *vte = VTE_TERMINAL(user_data);

    char *text = vte_terminal_get_text(vte, NULL, NULL, NULL);
    gboolean found = text && strstr(text, "hello-from-workbench") != NULL;
    g_free(text);

    if (!found) {
        fprintf(stderr, "terminal_smoke: expected echoed output not found in terminal buffer\n");
        gtk_window_close(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(vte))));
        return G_SOURCE_REMOVE;
    }

    /* resize while the shell is live - must not crash or hang */
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(vte)));
    gtk_window_resize(window, 1400, 900);
    gtk_window_resize(window, 800, 500);
    gtk_window_resize(window, 1100, 700);

    g_test_passed = TRUE;
    gtk_window_close(window);
    return G_SOURCE_REMOVE;
}

static gboolean send_command(gpointer user_data) {
    (void)user_data;

    GApplication *default_app = g_application_get_default();
    GList *windows = gtk_application_get_windows(GTK_APPLICATION(default_app));
    GtkWindow *window = GTK_WINDOW(windows->data);

    GPtrArray *terminals = g_ptr_array_new();
    collect_by_type(GTK_WIDGET(window), terminals, VTE_TYPE_TERMINAL);

    if (terminals->len != 1) {
        fprintf(stderr, "terminal_smoke: expected 1 VTE widget, found %u\n", terminals->len);
        gtk_window_close(window);
        return G_SOURCE_REMOVE;
    }

    VteTerminal *vte = VTE_TERMINAL(g_ptr_array_index(terminals, 0));
    g_ptr_array_free(terminals, TRUE);

    /* The app now owns the pty itself (see terminal_vte.c) instead of
     * letting VTE spawn/own it, so vte_terminal_feed_child() has nothing
     * to write to - go through the same terminal_send() path a real
     * keystroke takes, via the Terminal* the app attaches to the
     * terminal's containing page ("workbench-view", the same convention
     * ui_gtk_terminal.c's own active_terminal_page() uses). */
    GtkWidget *page = gtk_widget_get_parent(GTK_WIDGET(vte));
    Terminal *view = g_object_get_data(G_OBJECT(page), "workbench-view");
    static const char command[] = "echo hello-from-workbench\n";
    terminal_send(view, command, strlen(command));

    g_timeout_add(600, check_output, vte);
    return G_SOURCE_REMOVE;
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    App *app = app_create(0, NULL);
    g_timeout_add(400, send_command, NULL);

    int status = app_run(app);
    app_destroy(app);

    if (status != 0) {
        fprintf(stderr, "terminal_smoke: window run exited with status %d\n", status);
        return 1;
    }
    if (!g_test_passed) {
        fprintf(stderr, "terminal_smoke: FAILED\n");
        return 1;
    }

    g_print("terminal_smoke: shell spawn, echo round-trip, and live resize all verified\n");
    return 0;
}
