/*
 * Exercises Milestone 5's definition of done end to end: opening several
 * terminal tabs each with an independent shell, using them without cross-
 * talk, closing one selectively (preserving the others and reassigning the
 * active tab sensibly), and handling a shell exiting on its own without
 * the app crashing or leaving the tab in a broken state. GLib criticals
 * are promoted to fatal so a broken widget tree aborts the test instead
 * of just logging a warning.
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <vte/vte.h>

#include "app/app.h"
#include "terminal/terminal.h"
#include "test_gtk_utils.h"

static gboolean g_test_passed = FALSE;
static GtkWindow *g_window = NULL;

#define FAIL(msg)                                    \
    do {                                              \
        fprintf(stderr, "multi_terminal_smoke: %s\n", msg); \
        gtk_window_close(g_window);                  \
        return G_SOURCE_REMOVE;                      \
    } while (0)

static GtkWidget *find_notebook(GtkWidget *root) {
    GPtrArray *found = g_ptr_array_new();
    collect_by_type(root, found, GTK_TYPE_NOTEBOOK);
    GtkWidget *notebook = found->len > 0 ? GTK_WIDGET(g_ptr_array_index(found, 0)) : NULL;
    g_ptr_array_free(found, TRUE);
    return notebook;
}

static VteTerminal *terminal_for_page(GtkNotebook *notebook, gint index) {
    GtkWidget *page = gtk_notebook_get_nth_page(notebook, index);
    if (!page) {
        return NULL;
    }
    GPtrArray *found = g_ptr_array_new();
    collect_by_type(page, found, VTE_TYPE_TERMINAL);
    VteTerminal *vte = found->len == 1 ? VTE_TERMINAL(g_ptr_array_index(found, 0)) : NULL;
    g_ptr_array_free(found, TRUE);
    return vte;
}

static GtkWidget *find_close_button(GtkNotebook *notebook, GtkWidget *page) {
    GtkWidget *tab_label = gtk_notebook_get_tab_label(notebook, page);
    GPtrArray *found = g_ptr_array_new();
    collect_by_type(tab_label, found, GTK_TYPE_BUTTON);
    GtkWidget *button = found->len == 1 ? GTK_WIDGET(g_ptr_array_index(found, 0)) : NULL;
    g_ptr_array_free(found, TRUE);
    return button;
}

static gboolean text_contains(VteTerminal *vte, const char *needle) {
    char *text = vte_terminal_get_text(vte, NULL, NULL, NULL);
    gboolean found = text && strstr(text, needle) != NULL;
    g_free(text);
    return found;
}

/* The app now owns the pty itself (see terminal_vte.c) instead of letting
 * VTE spawn/own it, so vte_terminal_feed_child() has nothing to write to -
 * go through the same terminal_send() path a real keystroke takes, via
 * the Terminal* the app attaches to the terminal's containing page
 * ("toolbox-view", the same convention ui_gtk_terminal.c's own
 * active_terminal_page() uses). */
static void send(VteTerminal *vte, const char *command) {
    GtkWidget *page = gtk_widget_get_parent(GTK_WIDGET(vte));
    Terminal *view = g_object_get_data(G_OBJECT(page), "toolbox-view");
    terminal_send(view, command, strlen(command));
}

static gboolean phase_e_finish(gpointer user_data) {
    GtkNotebook *notebook = GTK_NOTEBOOK(user_data);

    VteTerminal *survivor = terminal_for_page(notebook, 1);
    if (!survivor || !text_contains(survivor, "marker-4")) {
        FAIL("surviving terminal did not respond after its sibling's shell exited");
    }
    if (gtk_notebook_get_n_pages(notebook) != 2) {
        FAIL("exited tab should remain open until explicitly closed");
    }

    g_test_passed = TRUE;
    gtk_window_close(g_window);
    return G_SOURCE_REMOVE;
}

static gboolean phase_d_verify_exit(gpointer user_data) {
    GtkNotebook *notebook = GTK_NOTEBOOK(user_data);

    VteTerminal *exited = terminal_for_page(notebook, 0);
    if (!exited || !text_contains(exited, "[Process exited]")) {
        FAIL("expected [Process exited] after the shell ran `exit`");
    }

    /* the app must still be fully functional for the other tab */
    VteTerminal *survivor = terminal_for_page(notebook, 1);
    if (!survivor) {
        FAIL("surviving terminal disappeared after sibling's shell exited");
    }
    send(survivor, "echo marker-4\n");

    g_timeout_add(500, phase_e_finish, notebook);
    return G_SOURCE_REMOVE;
}

static gboolean phase_c_trigger_exit(gpointer user_data) {
    GtkNotebook *notebook = GTK_NOTEBOOK(user_data);

    if (gtk_notebook_get_current_page(notebook) < 0) {
        FAIL("no active tab after closing the previously-active tab");
    }

    VteTerminal *term_a = terminal_for_page(notebook, 0);
    VteTerminal *term_c = terminal_for_page(notebook, 1);
    if (!term_a || !text_contains(term_a, "marker-1")) {
        FAIL("Terminal 1's output was lost after closing Terminal 2");
    }
    if (!term_c || !text_contains(term_c, "marker-3")) {
        FAIL("Terminal 3's output was lost after closing Terminal 2");
    }

    send(term_a, "exit\n");

    g_timeout_add(700, phase_d_verify_exit, notebook);
    return G_SOURCE_REMOVE;
}

static gboolean phase_b_close_middle(gpointer user_data) {
    GtkNotebook *notebook = GTK_NOTEBOOK(user_data);

    VteTerminal *term0 = terminal_for_page(notebook, 0);
    VteTerminal *term1 = terminal_for_page(notebook, 1);
    VteTerminal *term2 = terminal_for_page(notebook, 2);
    if (!term0 || !term1 || !term2) {
        FAIL("expected 3 independent terminal widgets");
    }
    if (!text_contains(term0, "marker-1") || text_contains(term0, "marker-2") ||
        text_contains(term0, "marker-3")) {
        FAIL("Terminal 1 shows cross-talk from another tab's shell");
    }
    if (!text_contains(term1, "marker-2") || text_contains(term1, "marker-1") ||
        text_contains(term1, "marker-3")) {
        FAIL("Terminal 2 shows cross-talk from another tab's shell");
    }
    if (!text_contains(term2, "marker-3") || text_contains(term2, "marker-1") ||
        text_contains(term2, "marker-2")) {
        FAIL("Terminal 3 shows cross-talk from another tab's shell");
    }

    /* close the currently-active middle tab and make sure a neighbor
     * becomes active instead of leaving no active tab at all */
    gtk_notebook_set_current_page(notebook, 1);
    GtkWidget *middle_page = gtk_notebook_get_nth_page(notebook, 1);
    GtkWidget *close_button = find_close_button(notebook, middle_page);
    if (!close_button) {
        FAIL("could not find Terminal 2's close button");
    }
    gtk_button_clicked(GTK_BUTTON(close_button));

    if (gtk_notebook_get_n_pages(notebook) != 2) {
        FAIL("expected 2 tabs after closing Terminal 2");
    }

    g_timeout_add(300, phase_c_trigger_exit, notebook);
    return G_SOURCE_REMOVE;
}

static gboolean phase_a_create_and_use(gpointer user_data) {
    (void)user_data;

    GApplication *default_app = g_application_get_default();
    GList *windows = gtk_application_get_windows(GTK_APPLICATION(default_app));
    g_window = GTK_WINDOW(windows->data);

    GtkWidget *notebook_widget = find_notebook(GTK_WIDGET(g_window));
    if (!notebook_widget) {
        FAIL("no notebook found");
    }
    GtkNotebook *notebook = GTK_NOTEBOOK(notebook_widget);

    if (gtk_notebook_get_n_pages(notebook) != 1) {
        FAIL("expected 1 seeded tab");
    }

    GtkWidget *add_button = gtk_notebook_get_action_widget(notebook, GTK_PACK_END);
    if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(add_button)), "+ Terminal") != 0) {
        FAIL("add-tab button should be labeled '+ Terminal'");
    }
    gtk_button_clicked(GTK_BUTTON(add_button));
    gtk_button_clicked(GTK_BUTTON(add_button));

    if (gtk_notebook_get_n_pages(notebook) != 3) {
        FAIL("expected 3 tabs after clicking '+ Terminal' twice");
    }

    VteTerminal *term0 = terminal_for_page(notebook, 0);
    VteTerminal *term1 = terminal_for_page(notebook, 1);
    VteTerminal *term2 = terminal_for_page(notebook, 2);
    if (!term0 || !term1 || !term2) {
        FAIL("expected 3 independent terminal widgets");
    }
    if (term0 == term1 || term1 == term2 || term0 == term2) {
        FAIL("tabs are sharing the same terminal widget");
    }

    send(term0, "echo marker-1\n");
    send(term1, "echo marker-2\n");
    send(term2, "echo marker-3\n");

    g_timeout_add(700, phase_b_close_middle, notebook);
    return G_SOURCE_REMOVE;
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    App *app = app_create(0, NULL);
    g_timeout_add(400, phase_a_create_and_use, NULL);

    int status = app_run(app);
    app_destroy(app);

    if (status != 0) {
        fprintf(stderr, "multi_terminal_smoke: window run exited with status %d\n", status);
        return 1;
    }
    if (!g_test_passed) {
        fprintf(stderr, "multi_terminal_smoke: FAILED\n");
        return 1;
    }

    g_print("multi_terminal_smoke: independent tabs, selective close, and shell-exit handling all verified\n");
    return 0;
}
