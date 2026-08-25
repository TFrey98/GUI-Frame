/*
 * Exercises the "terminal as a persistent Objects-panel object" feature:
 * a terminal tab's x undocks it from the notebook rather than destroying
 * it - its shell, TerminalSession, and scrollback all stay alive,
 * tracked as a Terminal row in the bottom Objects panel - and only that
 * row's own Close action actually ends the session. Covers: undock on
 * close (notebook page gone, shell still tracked), the Objects panel row
 * appearing RUNNING, its context menu (Open/Close), double-click
 * re-docking the *same* page (scrollback intact - proof it's the same
 * session, not a fresh shell), and Close (with its running-shell
 * confirmation) actually ending it (row gone, page gone).
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <vte/vte.h>

#include "app/app.h"
#include "core/tab.h"
#include "terminal/terminal.h"
#include "test_gtk_utils.h"

enum {
    OBJECT_PANEL_COL_NAME,
    OBJECT_PANEL_COL_ENDPOINT,
    OBJECT_PANEL_COL_STATE,
    OBJECT_PANEL_COL_ID,
    OBJECT_PANEL_COL_KIND
};

enum { OBJECT_PANEL_KIND_LISTENER, OBJECT_PANEL_KIND_CONNECTION, OBJECT_PANEL_KIND_TERMINAL };

#define STEP_INTERVAL_MS 100
#define STEP_TIMEOUT_MS 4000
#define MARKER "marker-terminal-object"

typedef enum Step {
    STEP_WAIT_STARTUP,
    STEP_SEND_MARKER,
    STEP_WAIT_MARKER,
    STEP_CLOSE_TAB,
    STEP_WAIT_OBJECT_ROW,
    STEP_CHECK_MENU_BEFORE_REOPEN,
    STEP_REOPEN,
    STEP_WAIT_REDOCKED,
    STEP_CLICK_CLOSE,
    STEP_WAIT_CONFIRM_DIALOG,
    STEP_WAIT_TRULY_GONE
} Step;

typedef struct TestState {
    Step step;
    int step_elapsed_ms;
    gboolean failed;
    gboolean done;
    guint64 terminal_row_id;
} TestState;

static GtkWidget *find_by_data_key(GtkWidget *widget, const char *key) {
    if (g_object_get_data(G_OBJECT(widget), key)) {
        return widget;
    }
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *l = children; l; l = l->next) {
            GtkWidget *found = find_by_data_key(GTK_WIDGET(l->data), key);
            if (found) {
                g_list_free(children);
                return found;
            }
        }
        g_list_free(children);
    }
    return NULL;
}

static GtkWindow *main_window(void) {
    GApplication *default_app = g_application_get_default();
    if (!default_app) {
        return NULL;
    }
    GList *windows = gtk_application_get_windows(GTK_APPLICATION(default_app));
    return windows ? GTK_WINDOW(windows->data) : NULL;
}

static GtkWidget *find_notebook(GtkWidget *window) {
    GPtrArray *found = g_ptr_array_new();
    collect_by_type(window, found, GTK_TYPE_NOTEBOOK);
    GtkWidget *notebook = found->len > 0 ? GTK_WIDGET(g_ptr_array_index(found, 0)) : NULL;
    g_ptr_array_free(found, TRUE);
    return notebook;
}

static GtkWidget *object_panel_tree(GtkWidget *window) {
    return find_by_data_key(window, "workbench-object-panel-tree");
}

static gboolean find_terminal_row(GtkTreeModel *model, GtkTreeIter *out_iter) {
    if (!model) {
        return FALSE;
    }
    GtkTreeIter iter;
    gboolean has = gtk_tree_model_get_iter_first(model, &iter);
    while (has) {
        int kind = OBJECT_PANEL_KIND_LISTENER;
        gtk_tree_model_get(model, &iter, OBJECT_PANEL_COL_KIND, &kind, -1);
        if (kind == OBJECT_PANEL_KIND_TERMINAL) {
            *out_iter = iter;
            return TRUE;
        }
        has = gtk_tree_model_iter_next(model, &iter);
    }
    return FALSE;
}

static gboolean row_state_is(GtkTreeModel *model, GtkTreeIter *iter, const char *expected) {
    gchar *state = NULL;
    gtk_tree_model_get(model, iter, OBJECT_PANEL_COL_STATE, &state, -1);
    gboolean match = state && strcmp(state, expected) == 0;
    g_free(state);
    return match;
}

static guint64 row_id(GtkTreeModel *model, GtkTreeIter *iter) {
    guint64 id = 0;
    gtk_tree_model_get(model, iter, OBJECT_PANEL_COL_ID, &id, -1);
    return id;
}

static VteTerminal *vte_for_page(GtkWidget *page) {
    GPtrArray *found = g_ptr_array_new();
    collect_by_type(page, found, VTE_TYPE_TERMINAL);
    VteTerminal *vte = found->len == 1 ? VTE_TERMINAL(g_ptr_array_index(found, 0)) : NULL;
    g_ptr_array_free(found, TRUE);
    return vte;
}

static gboolean text_contains(VteTerminal *vte, const char *needle) {
    char *text = vte_terminal_get_text(vte, NULL, NULL, NULL);
    gboolean found = text && strstr(text, needle) != NULL;
    g_free(text);
    return found;
}

/* Goes through terminal_send(), same as a real keystroke - see
 * multi_terminal_smoke.c's own send() for why vte_terminal_feed_child()
 * doesn't work here (the app owns the pty, not VTE). */
static void send_command(VteTerminal *vte, const char *command) {
    GtkWidget *page = gtk_widget_get_parent(GTK_WIDGET(vte));
    Terminal *view = g_object_get_data(G_OBJECT(page), "workbench-view");
    terminal_send(view, command, strlen(command));
}

static GtkWidget *find_close_button(GtkNotebook *notebook, GtkWidget *page) {
    GtkWidget *tab_label = gtk_notebook_get_tab_label(notebook, page);
    GPtrArray *found = g_ptr_array_new();
    collect_by_type(tab_label, found, GTK_TYPE_BUTTON);
    GtkWidget *button = found->len == 1 ? GTK_WIDGET(g_ptr_array_index(found, 0)) : NULL;
    g_ptr_array_free(found, TRUE);
    return button;
}

static GtkWidget *find_transient_dialog(GtkWindow *main_win) {
    GList *toplevels = gtk_window_list_toplevels();
    GtkWidget *found = NULL;
    for (GList *l = toplevels; l; l = l->next) {
        GtkWidget *w = GTK_WIDGET(l->data);
        if (w != GTK_WIDGET(main_win) && GTK_IS_DIALOG(w) && gtk_widget_get_visible(w)) {
            found = w;
            break;
        }
    }
    g_list_free(toplevels);
    return found;
}

static GtkWidget *open_menu_for_row(GtkWidget *tree_view, GtkTreeModel *model, GtkTreeIter *iter) {
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view)), path);
    gtk_tree_path_free(path);
    gboolean handled = FALSE;
    g_signal_emit_by_name(tree_view, "popup-menu", &handled);
    return g_object_get_data(G_OBJECT(tree_view), "workbench-object-context-menu");
}

static gboolean find_menu_item_sensitive(GtkWidget *menu, const char *label, gboolean *out_sensitive) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
    gboolean found = FALSE;
    for (GList *l = children; l; l = l->next) {
        GtkWidget *item = GTK_WIDGET(l->data);
        if (!GTK_IS_MENU_ITEM(item)) {
            continue;
        }
        GtkWidget *label_widget = gtk_bin_get_child(GTK_BIN(item));
        if (GTK_IS_LABEL(label_widget) && strcmp(gtk_label_get_text(GTK_LABEL(label_widget)), label) == 0) {
            *out_sensitive = gtk_widget_get_sensitive(item);
            found = TRUE;
            break;
        }
    }
    g_list_free(children);
    return found;
}

static void click_menu_item(GtkWidget *menu, const char *label) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *l = children; l; l = l->next) {
        GtkWidget *item = GTK_WIDGET(l->data);
        if (!GTK_IS_MENU_ITEM(item)) {
            continue;
        }
        GtkWidget *label_widget = gtk_bin_get_child(GTK_BIN(item));
        if (GTK_IS_LABEL(label_widget) && strcmp(gtk_label_get_text(GTK_LABEL(label_widget)), label) == 0) {
            g_signal_emit_by_name(item, "activate");
            break;
        }
    }
    g_list_free(children);
}

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "terminal_object_panel_smoke: %s\n", msg);
    test->failed = TRUE;
    GtkWindow *w = main_window();
    if (w) {
        gtk_window_close(w);
    }
}

static gboolean drive(gpointer user_data) {
    TestState *test = user_data;

    for (;;) {
        GtkWindow *window = main_window();
        if (!window) {
            return G_SOURCE_REMOVE;
        }
        GtkWidget *notebook = find_notebook(GTK_WIDGET(window));
        GtkWidget *tree_view = object_panel_tree(GTK_WIDGET(window));
        GtkTreeModel *model = tree_view ? gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view)) : NULL;

        switch (test->step) {
            case STEP_WAIT_STARTUP: {
                if (!notebook || gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) != 1) {
                    break;
                }
                GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), 0);
                if (!vte_for_page(page)) {
                    break;
                }
                test->step = STEP_SEND_MARKER;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_SEND_MARKER: {
                GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), 0);
                send_command(vte_for_page(page), "echo " MARKER "\n");
                test->step = STEP_WAIT_MARKER;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_MARKER: {
                GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), 0);
                if (text_contains(vte_for_page(page), MARKER)) {
                    test->step = STEP_CLOSE_TAB;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_CLOSE_TAB: {
                GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), 0);
                GtkWidget *close_button = find_close_button(GTK_NOTEBOOK(notebook), page);
                if (!close_button) {
                    fail(test, "could not find the startup terminal's close button");
                    return G_SOURCE_REMOVE;
                }
                gtk_button_clicked(GTK_BUTTON(close_button));

                if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) != 0) {
                    fail(test, "closing a terminal tab should undock its page immediately");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_OBJECT_ROW;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_OBJECT_ROW: {
                GtkTreeIter iter;
                if (model && find_terminal_row(model, &iter)) {
                    if (!row_state_is(model, &iter, "RUNNING")) {
                        fail(test, "the undocked terminal's object row should show RUNNING");
                        return G_SOURCE_REMOVE;
                    }
                    test->terminal_row_id = row_id(model, &iter);
                    test->step = STEP_CHECK_MENU_BEFORE_REOPEN;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_CHECK_MENU_BEFORE_REOPEN: {
                GtkTreeIter iter;
                if (!find_terminal_row(model, &iter)) {
                    fail(test, "terminal row disappeared before its menu could be checked");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *menu = open_menu_for_row(tree_view, model, &iter);
                if (!menu) {
                    fail(test, "no context menu appeared for the terminal row");
                    return G_SOURCE_REMOVE;
                }
                gboolean open_sensitive = FALSE, close_sensitive = FALSE;
                if (!find_menu_item_sensitive(menu, "Open", &open_sensitive) ||
                    !find_menu_item_sensitive(menu, "Close", &close_sensitive)) {
                    fail(test, "terminal row's menu should offer Open and Close");
                    return G_SOURCE_REMOVE;
                }
                if (!open_sensitive || !close_sensitive) {
                    fail(test, "Open/Close should both be sensitive for a running undocked terminal");
                    return G_SOURCE_REMOVE;
                }
                gtk_menu_popdown(GTK_MENU(menu));
                test->step = STEP_REOPEN;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_REOPEN: {
                GtkTreeIter iter;
                if (!find_terminal_row(model, &iter)) {
                    fail(test, "terminal row disappeared before it could be reopened");
                    return G_SOURCE_REMOVE;
                }
                GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
                gtk_tree_view_row_activated(GTK_TREE_VIEW(tree_view), path,
                                             gtk_tree_view_get_column(GTK_TREE_VIEW(tree_view), 0));
                gtk_tree_path_free(path);
                test->step = STEP_WAIT_REDOCKED;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_REDOCKED: {
                if (!notebook || gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) != 1) {
                    break;
                }
                GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), 0);
                VteTerminal *vte = vte_for_page(page);
                if (!vte) {
                    break;
                }
                if (!text_contains(vte, MARKER)) {
                    fail(test, "re-docked terminal lost its scrollback - a fresh shell was opened instead of the "
                               "original session");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_CLICK_CLOSE;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_CLICK_CLOSE: {
                GtkTreeIter iter;
                if (!find_terminal_row(model, &iter)) {
                    fail(test, "terminal row disappeared before Close could be clicked");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *menu = open_menu_for_row(tree_view, model, &iter);
                if (!menu) {
                    fail(test, "no context menu appeared for the re-docked terminal row");
                    return G_SOURCE_REMOVE;
                }
                click_menu_item(menu, "Close");
                test->step = STEP_WAIT_CONFIRM_DIALOG;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_CONFIRM_DIALOG: {
                GtkWidget *dialog = find_transient_dialog(window);
                if (dialog) {
                    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
                    test->step = STEP_WAIT_TRULY_GONE;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_WAIT_TRULY_GONE: {
                GtkTreeIter iter;
                gboolean row_gone = !(model && find_terminal_row(model, &iter));
                gboolean page_gone = !notebook || gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) == 0;
                if (row_gone && page_gone) {
                    test->done = TRUE;
                    gtk_window_close(window);
                    return G_SOURCE_REMOVE;
                }
                break;
            }
        }

        break; /* the current step is a wait that isn't satisfied yet */
    }

    test->step_elapsed_ms += STEP_INTERVAL_MS;
    if (test->step_elapsed_ms >= STEP_TIMEOUT_MS) {
        char msg[64];
        snprintf(msg, sizeof(msg), "timed out waiting on step %d", (int)test->step);
        fail(test, msg);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);

    g_timeout_add(STEP_INTERVAL_MS, drive, &test);

    int status = app_run(app);
    app_destroy(app);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "terminal_object_panel_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "terminal_object_panel_smoke: test did not complete\n");
        return 1;
    }

    g_print("terminal_object_panel_smoke: undock-not-destroy on close, Objects panel tracking, Open/Close menu, "
            "scrollback-preserving re-dock, and Close ending the session all verified\n");
    return 0;
}
