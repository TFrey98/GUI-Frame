/*
 * Exercises Phase 10's checkpoint end-to-end in the real app: "every
 * menu item's enabled state matches its predicate across the state
 * matrix," plus the two new guarded flows (remove, close-running-tab).
 * Doesn't re-prove the predicates themselves - object_predicates_test.c
 * already covers the full state matrix exhaustively - proves the menu
 * is wired to them correctly.
 *
 * Three listeners, each used for a different slice of the checkpoint:
 * A (port 4444) - Start/Stop/Restart sensitivity across RUNNING/STOPPED,
 * then immediate (unguarded) remove once STOPPED. B (port 4445) - a
 * real connection's Open Terminal/Wait for Reconnection/Stop
 * sensitivity across CONNECTED/DISCONNECTED, then guarded remove while
 * still RUNNING (confirmation dialog). D (port 4446) - the
 * close-running-tab three-button confirmation.
 */
#include <arpa/inet.h>
#include <gtk/gtk.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "app/app.h"
#include "core/tab.h"
#include "listeners/listener_manager.h"
#include "test_gtk_utils.h"

#define STEP_INTERVAL_MS 100
#define STEP_TIMEOUT_MS 3000

enum {
    OBJECT_PANEL_COL_NAME,
    OBJECT_PANEL_COL_ENDPOINT,
    OBJECT_PANEL_COL_STATE,
    OBJECT_PANEL_COL_ID
};

typedef enum Step {
    STEP_OPEN_DIALOG_A,
    STEP_WAIT_A_RUNNING,
    STEP_CHECK_MENU_A_RUNNING,
    STEP_STOP_A,
    STEP_WAIT_A_STOPPED,
    STEP_CHECK_MENU_A_STOPPED,
    STEP_REMOVE_A_IMMEDIATE,
    STEP_VERIFY_A_GONE,

    STEP_OPEN_DIALOG_B,
    STEP_WAIT_B_RUNNING,
    STEP_OPEN_CLIENT_SOCKET_B,
    STEP_WAIT_B_CONNECTION_CONNECTED,
    STEP_CHECK_MENU_CONNECTION_CONNECTED,
    STEP_DISCONNECT_CLIENT_B,
    STEP_WAIT_B_CONNECTION_DISCONNECTED,
    STEP_CHECK_MENU_CONNECTION_DISCONNECTED,
    STEP_OPEN_MENU_B_CLICK_REMOVE,
    STEP_WAIT_REMOVE_B_DIALOG,
    STEP_WAIT_B_GONE,

    STEP_OPEN_DIALOG_D,
    STEP_WAIT_D_RUNNING,
    STEP_CLOSE_D_TAB,
    STEP_WAIT_CLOSE_D_DIALOG,
    STEP_WAIT_D_TAB_GONE_AND_STOPPED
} Step;

typedef struct TestState {
    Step step;
    int step_elapsed_ms;
    gboolean failed;
    gboolean done;
    App *app;
    int client_fd;
    uint64_t listener_a_id;
    uint64_t listener_b_id;
    uint64_t listener_d_id;
    uint64_t connection_b_id;
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

static GtkWidget *find_new_listener_dialog(void) {
    GList *toplevels = gtk_window_list_toplevels();
    GtkWidget *found = NULL;
    for (GList *l = toplevels; l; l = l->next) {
        GtkWidget *w = GTK_WIDGET(l->data);
        if (g_object_get_data(G_OBJECT(w), "toolbox-new-listener-dialog")) {
            found = w;
            break;
        }
    }
    g_list_free(toplevels);
    return found;
}

/* The close-running-tab and remove confirmations are plain
 * GtkMessageDialogs, not self-tagged like the New Listener dialog -
 * found instead as "whichever visible GtkDialog toplevel isn't the
 * main window," which is unambiguous since at most one is ever open
 * at the points this test looks for one. */
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

static GtkWidget *find_notebook(GtkWidget *window) {
    GPtrArray *found = g_ptr_array_new();
    collect_by_type(window, found, GTK_TYPE_NOTEBOOK);
    GtkWidget *notebook = found->len > 0 ? GTK_WIDGET(g_ptr_array_index(found, 0)) : NULL;
    g_ptr_array_free(found, TRUE);
    return notebook;
}

static GtkWidget *object_panel_tree(GtkWidget *window) {
    return find_by_data_key(window, "toolbox-object-panel-tree");
}

static gboolean find_row_by_name(GtkTreeModel *model, const char *name, GtkTreeIter *out_iter) {
    GtkTreeIter iter;
    gboolean has = gtk_tree_model_get_iter_first(model, &iter);
    while (has) {
        gchar *row_name = NULL;
        gtk_tree_model_get(model, &iter, OBJECT_PANEL_COL_NAME, &row_name, -1);
        gboolean match = row_name && strcmp(row_name, name) == 0;
        g_free(row_name);
        if (match) {
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

typedef struct ExpectedItem {
    const char *label;
    gboolean sensitive;
} ExpectedItem;

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

static gboolean menu_matches(GtkWidget *menu, const ExpectedItem *expected, int count, char *err, size_t err_len) {
    for (int i = 0; i < count; i++) {
        gboolean sensitive = FALSE;
        if (!find_menu_item_sensitive(menu, expected[i].label, &sensitive)) {
            snprintf(err, err_len, "menu item '%s' not found", expected[i].label);
            return FALSE;
        }
        if (sensitive != expected[i].sensitive) {
            snprintf(err, err_len, "menu item '%s' sensitivity mismatch (expected %d, got %d)", expected[i].label,
                     expected[i].sensitive, sensitive);
            return FALSE;
        }
    }
    return TRUE;
}

/* "popup-menu" is a real, standard GTK signal (like row-activated) -
 * emitting it programmatically is the keyboard-context-menu trigger
 * GTK itself defines (e.g. Shift+F10), not a simulation of raw input. */
static GtkWidget *open_menu_for_row(GtkWidget *tree_view, GtkTreeModel *model, GtkTreeIter *iter) {
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    /* gtk_tree_selection_select_path() silently fails to select a
     * child row that isn't currently expanded/visible - expand its
     * ancestors first so selection (and the menu that follows) targets
     * the row this call actually asked for. */
    gtk_tree_view_expand_to_path(GTK_TREE_VIEW(tree_view), path);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view)), path);
    gtk_tree_path_free(path);
    gboolean handled = FALSE;
    g_signal_emit_by_name(tree_view, "popup-menu", &handled);
    return g_object_get_data(G_OBJECT(tree_view), "toolbox-object-context-menu");
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

static gboolean submit_new_listener(GtkWindow *window, const char *name, const char *port) {
    GtkWidget *new_button = find_by_data_key(GTK_WIDGET(window), "toolbox-new-listener-button");
    if (!new_button) {
        return FALSE;
    }
    gtk_button_clicked(GTK_BUTTON(new_button));

    GtkWidget *dialog = find_new_listener_dialog();
    if (!dialog) {
        return FALSE;
    }
    GtkWidget *name_entry = find_by_data_key(dialog, "toolbox-listener-name-entry");
    GtkWidget *callback_entry = find_by_data_key(dialog, "toolbox-listener-callback-host-entry");
    GtkWidget *port_entry = find_by_data_key(dialog, "toolbox-listener-port-entry");
    if (!name_entry || !callback_entry || !port_entry) {
        return FALSE;
    }
    gtk_entry_set_text(GTK_ENTRY(name_entry), name);
    gtk_entry_set_text(GTK_ENTRY(callback_entry), "127.0.0.1");
    if (port) {
        gtk_entry_set_text(GTK_ENTRY(port_entry), port);
    }
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    return TRUE;
}

static int connect_client_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "context_menu_smoke: %s\n", msg);
    test->failed = TRUE;
    if (test->client_fd >= 0) {
        close(test->client_fd);
        test->client_fd = -1;
    }
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
            case STEP_OPEN_DIALOG_A: {
                if (!find_by_data_key(GTK_WIDGET(window), "toolbox-new-listener-button")) {
                    break; /* window may not have finished its initial build yet */
                }
                if (!submit_new_listener(window, "MenuSmokeA", NULL)) {
                    fail(test, "could not submit listener A");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_A_RUNNING;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_A_RUNNING: {
                GtkTreeIter iter;
                if (model && find_row_by_name(model, "MenuSmokeA", &iter) && row_state_is(model, &iter, "RUNNING")) {
                    test->listener_a_id = row_id(model, &iter);
                    test->step = STEP_CHECK_MENU_A_RUNNING;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_CHECK_MENU_A_RUNNING: {
                GtkTreeIter iter;
                if (!find_row_by_name(model, "MenuSmokeA", &iter)) {
                    fail(test, "listener A row disappeared");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *menu = open_menu_for_row(tree_view, model, &iter);
                if (!menu) {
                    fail(test, "no context menu appeared for listener A (RUNNING)");
                    return G_SOURCE_REMOVE;
                }
                static const ExpectedItem expected[] = {
                    {"Start", FALSE}, {"Stop", TRUE}, {"Restart", TRUE},
                    {"Open Terminal", FALSE}, {"Wait for Reconnection", FALSE}, {"Remove", TRUE},
                };
                char err[128];
                if (!menu_matches(menu, expected, 6, err, sizeof(err))) {
                    fail(test, err);
                    return G_SOURCE_REMOVE;
                }
                gtk_menu_popdown(GTK_MENU(menu));
                test->step = STEP_STOP_A;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_STOP_A: {
                ListenerSystem *system = app_get_listener_system(test->app);
                listener_manager_stop(system->listener_manager, test->listener_a_id);
                test->step = STEP_WAIT_A_STOPPED;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_A_STOPPED: {
                GtkTreeIter iter;
                if (model && find_row_by_name(model, "MenuSmokeA", &iter) && row_state_is(model, &iter, "STOPPED")) {
                    test->step = STEP_CHECK_MENU_A_STOPPED;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_CHECK_MENU_A_STOPPED: {
                GtkTreeIter iter;
                if (!find_row_by_name(model, "MenuSmokeA", &iter)) {
                    fail(test, "listener A row disappeared");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *menu = open_menu_for_row(tree_view, model, &iter);
                if (!menu) {
                    fail(test, "no context menu appeared for listener A (STOPPED)");
                    return G_SOURCE_REMOVE;
                }
                static const ExpectedItem expected[] = {
                    {"Start", TRUE}, {"Stop", FALSE}, {"Restart", TRUE}, {"Remove", TRUE},
                };
                char err[128];
                if (!menu_matches(menu, expected, 4, err, sizeof(err))) {
                    fail(test, err);
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_REMOVE_A_IMMEDIATE;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_REMOVE_A_IMMEDIATE: {
                GtkTreeIter iter;
                if (!find_row_by_name(model, "MenuSmokeA", &iter)) {
                    fail(test, "listener A row disappeared before Remove");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *menu = open_menu_for_row(tree_view, model, &iter);
                click_menu_item(menu, "Remove");

                if (find_transient_dialog(window)) {
                    fail(test, "removing a STOPPED listener should not show a confirmation dialog");
                    return G_SOURCE_REMOVE;
                }
                ListenerSystem *system = app_get_listener_system(test->app);
                if (object_registry_get_listener(system->registry, test->listener_a_id)) {
                    fail(test, "STOPPED listener A should have been removed immediately");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_VERIFY_A_GONE;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_VERIFY_A_GONE: {
                GtkTreeIter iter;
                if (!find_row_by_name(model, "MenuSmokeA", &iter)) {
                    test->step = STEP_OPEN_DIALOG_B;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break; /* wait for the next tick's refresh_object_panel to drop the row */
            }

            case STEP_OPEN_DIALOG_B: {
                if (!submit_new_listener(window, "MenuSmokeB", "4445")) {
                    fail(test, "could not submit listener B");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_B_RUNNING;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_B_RUNNING: {
                GtkTreeIter iter;
                if (model && find_row_by_name(model, "MenuSmokeB", &iter) && row_state_is(model, &iter, "RUNNING")) {
                    test->listener_b_id = row_id(model, &iter);
                    test->step = STEP_OPEN_CLIENT_SOCKET_B;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_OPEN_CLIENT_SOCKET_B: {
                test->client_fd = connect_client_socket(4445);
                if (test->client_fd < 0) {
                    fail(test, "client socket failed to connect to listener B");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_B_CONNECTION_CONNECTED;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_B_CONNECTION_CONNECTED: {
                GtkTreeIter listener_iter;
                if (model && find_row_by_name(model, "MenuSmokeB", &listener_iter)) {
                    GtkTreeIter child;
                    if (gtk_tree_model_iter_children(model, &child, &listener_iter) &&
                        row_state_is(model, &child, "CONNECTED")) {
                        test->connection_b_id = row_id(model, &child);
                        test->step = STEP_CHECK_MENU_CONNECTION_CONNECTED;
                        test->step_elapsed_ms = 0;
                        continue;
                    }
                }
                break;
            }

            case STEP_CHECK_MENU_CONNECTION_CONNECTED: {
                GtkTreeIter listener_iter, child;
                if (!find_row_by_name(model, "MenuSmokeB", &listener_iter) ||
                    !gtk_tree_model_iter_children(model, &child, &listener_iter)) {
                    fail(test, "connection row disappeared");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *menu = open_menu_for_row(tree_view, model, &child);
                if (!menu) {
                    fail(test, "no context menu appeared for connection (CONNECTED)");
                    return G_SOURCE_REMOVE;
                }
                static const ExpectedItem expected[] = {
                    {"Start", FALSE}, {"Stop", TRUE}, {"Restart", FALSE},
                    {"Open Terminal", TRUE}, {"Wait for Reconnection", FALSE}, {"Remove", TRUE},
                };
                char err[128];
                if (!menu_matches(menu, expected, 6, err, sizeof(err))) {
                    fail(test, err);
                    return G_SOURCE_REMOVE;
                }
                gtk_menu_popdown(GTK_MENU(menu));
                test->step = STEP_DISCONNECT_CLIENT_B;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_DISCONNECT_CLIENT_B: {
                close(test->client_fd);
                test->client_fd = -1;
                test->step = STEP_WAIT_B_CONNECTION_DISCONNECTED;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_B_CONNECTION_DISCONNECTED: {
                GtkTreeIter listener_iter;
                if (model && find_row_by_name(model, "MenuSmokeB", &listener_iter)) {
                    GtkTreeIter child;
                    if (gtk_tree_model_iter_children(model, &child, &listener_iter) &&
                        row_state_is(model, &child, "DISCONNECTED")) {
                        test->step = STEP_CHECK_MENU_CONNECTION_DISCONNECTED;
                        test->step_elapsed_ms = 0;
                        continue;
                    }
                }
                break;
            }

            case STEP_CHECK_MENU_CONNECTION_DISCONNECTED: {
                GtkTreeIter listener_iter, child;
                if (!find_row_by_name(model, "MenuSmokeB", &listener_iter) ||
                    !gtk_tree_model_iter_children(model, &child, &listener_iter)) {
                    fail(test, "connection row disappeared");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *menu = open_menu_for_row(tree_view, model, &child);
                if (!menu) {
                    fail(test, "no context menu appeared for connection (DISCONNECTED)");
                    return G_SOURCE_REMOVE;
                }
                static const ExpectedItem expected[] = {
                    {"Stop", FALSE}, {"Open Terminal", TRUE}, {"Wait for Reconnection", TRUE},
                };
                char err[128];
                if (!menu_matches(menu, expected, 3, err, sizeof(err))) {
                    fail(test, err);
                    return G_SOURCE_REMOVE;
                }
                gtk_menu_popdown(GTK_MENU(menu));
                test->step = STEP_OPEN_MENU_B_CLICK_REMOVE;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_OPEN_MENU_B_CLICK_REMOVE: {
                GtkTreeIter iter;
                if (!find_row_by_name(model, "MenuSmokeB", &iter)) {
                    fail(test, "listener B row disappeared");
                    return G_SOURCE_REMOVE;
                }
                if (!row_state_is(model, &iter, "RUNNING")) {
                    fail(test, "listener B should still be RUNNING (only its connection disconnected)");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *menu = open_menu_for_row(tree_view, model, &iter);
                click_menu_item(menu, "Remove");
                test->step = STEP_WAIT_REMOVE_B_DIALOG;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_REMOVE_B_DIALOG: {
                GtkWidget *dialog = find_transient_dialog(window);
                if (dialog) {
                    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
                    test->step = STEP_WAIT_B_GONE;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_WAIT_B_GONE: {
                ListenerSystem *system = app_get_listener_system(test->app);
                if (!object_registry_get_listener(system->registry, test->listener_b_id)) {
                    test->step = STEP_OPEN_DIALOG_D;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_OPEN_DIALOG_D: {
                if (!submit_new_listener(window, "MenuSmokeD", "4446")) {
                    fail(test, "could not submit listener D");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_D_RUNNING;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_D_RUNNING: {
                /* Also waits for D's own console tab (Phase 7) to
                 * auto-open - matched by listener id, not just type,
                 * since B's now-removed listener's console tab is still
                 * open too (removal never auto-closes tabs; they just
                 * show "State: gone", per Phase 7/9's existing
                 * gone-object handling). */
                GtkTreeIter iter;
                gboolean row_running = model && find_row_by_name(model, "MenuSmokeD", &iter) &&
                                       row_state_is(model, &iter, "RUNNING");
                if (row_running) {
                    test->listener_d_id = row_id(model, &iter);
                }
                gboolean tab_open = FALSE;
                if (notebook && test->listener_d_id != 0) {
                    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
                    for (int i = 0; i < n && !tab_open; i++) {
                        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
                        Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
                        tab_open = tab && tab->type == TAB_TYPE_LISTENER &&
                                   (uint64_t)(uintptr_t)tab->backend_data == test->listener_d_id;
                    }
                }
                if (row_running && tab_open) {
                    test->step = STEP_CLOSE_D_TAB;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_CLOSE_D_TAB: {
                GtkWidget *page = NULL;
                int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
                for (int i = 0; i < n; i++) {
                    GtkWidget *candidate = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
                    Tab *tab = g_object_get_data(G_OBJECT(candidate), "toolbox-tab");
                    if (tab && tab->type == TAB_TYPE_LISTENER &&
                        (uint64_t)(uintptr_t)tab->backend_data == test->listener_d_id) {
                        page = candidate;
                        break;
                    }
                }
                if (!page) {
                    fail(test, "listener D's console tab not found");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *tab_label = gtk_notebook_get_tab_label(GTK_NOTEBOOK(notebook), page);
                GPtrArray *buttons = g_ptr_array_new();
                collect_by_type(tab_label, buttons, GTK_TYPE_BUTTON);
                if (buttons->len == 0) {
                    g_ptr_array_free(buttons, TRUE);
                    fail(test, "could not find the tab's close button");
                    return G_SOURCE_REMOVE;
                }
                gtk_button_clicked(GTK_BUTTON(g_ptr_array_index(buttons, 0)));
                g_ptr_array_free(buttons, TRUE);

                test->step = STEP_WAIT_CLOSE_D_DIALOG;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_CLOSE_D_DIALOG: {
                GtkWidget *dialog = find_transient_dialog(window);
                if (dialog) {
                    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES); /* "Stop Listener" */
                    test->step = STEP_WAIT_D_TAB_GONE_AND_STOPPED;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_WAIT_D_TAB_GONE_AND_STOPPED: {
                gboolean tab_gone = TRUE;
                if (notebook) {
                    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
                    for (int i = 0; i < n; i++) {
                        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
                        Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
                        if (tab && tab->type == TAB_TYPE_LISTENER &&
                            (uint64_t)(uintptr_t)tab->backend_data == test->listener_d_id) {
                            tab_gone = FALSE;
                        }
                    }
                }
                GtkTreeIter iter;
                gboolean stopped =
                    model && find_row_by_name(model, "MenuSmokeD", &iter) && row_state_is(model, &iter, "STOPPED");
                if (tab_gone && stopped) {
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
    test.client_fd = -1;
    test.app = app_create(0, NULL);

    g_timeout_add(STEP_INTERVAL_MS, drive, &test);

    int status = app_run(test.app);
    app_destroy(test.app);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "context_menu_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "context_menu_smoke: test did not complete\n");
        return 1;
    }

    g_print("context_menu_smoke: menu sensitivities, guarded remove, and "
            "close-running-tab confirmation all verified\n");
    return 0;
}
