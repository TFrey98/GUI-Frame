/*
 * Exercises Phase 9's checkpoint end-to-end in the real app: "close
 * terminal -> connection stays Connected; reopen shows history +
 * resumes; two views -> only one can send; disconnected double-click ->
 * read-only previous output."
 *
 * Verifies at two layers rather than scraping VTE's own rendered text:
 * the data layer (terminal_history_len()/_read() on the registry's
 * Connection, reached via app_get_listener_system() - the same
 * verification connection_io_test.c already trusts) proves bytes really
 * flow end-to-end; the structural GUI layer (tab existence/type, label
 * text, button sensitivity) proves the view wiring is correct. No
 * existing test touches VTE's own API surface directly (only
 * ui_gtk.c/terminal_vte.c do), and this keeps that boundary intact.
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
#include "test_gtk_utils.h"

#define STEP_INTERVAL_MS 100
#define STEP_TIMEOUT_MS 3000
#define LISTENER_NAME "TermSmoke"
#define LISTENER_PORT 4444

/* Mirrors ui_gtk.c's file-local OBJECT_PANEL_COL_* enum - not a public
 * API, same convention every prior GTK smoke test uses for
 * ui_gtk.c-internal object data. */
enum {
    OBJECT_PANEL_COL_NAME,
    OBJECT_PANEL_COL_ENDPOINT,
    OBJECT_PANEL_COL_STATE,
    OBJECT_PANEL_COL_ID
};

typedef enum Step {
    STEP_OPEN_DIALOG,
    STEP_WAIT_LISTENER_RUNNING,
    STEP_OPEN_CLIENT_SOCKET,
    STEP_WAIT_CONNECTION_ROW,
    STEP_ACTIVATE_ROW_FIRST_OPEN,
    STEP_WAIT_PAGE_CONNECTED,
    STEP_SEND_FIRST_DATA,
    STEP_WAIT_HISTORY_GREW,
    STEP_CLOSE_TAB,
    STEP_VERIFY_STILL_CONNECTED,
    STEP_REOPEN_ROW,
    STEP_WAIT_REOPENED_PAGE,
    STEP_SEND_SECOND_DATA,
    STEP_WAIT_HISTORY_GREW_MORE,
    STEP_OPEN_ANOTHER_VIEW,
    STEP_WAIT_TWO_PAGES,
    STEP_TAKE_CONTROL_SECOND,
    STEP_WAIT_CONTROL_SWAPPED,
    STEP_DISCONNECT_CLIENT,
    STEP_WAIT_ROW_DISCONNECTED,
    STEP_ACTIVATE_ROW_AGAIN,
    STEP_VERIFY_DISCONNECTED_READONLY
} Step;

typedef struct TestState {
    Step step;
    int step_elapsed_ms;
    gboolean failed;
    gboolean done;
    App *app;
    int client_fd;
    uint64_t connection_id;
    size_t history_before_reopen;
    size_t history_before_disconnect;
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

static gboolean find_listener_row(GtkTreeModel *model, GtkTreeIter *out_iter) {
    GtkTreeIter iter;
    gboolean has = gtk_tree_model_get_iter_first(model, &iter);
    while (has) {
        gchar *name = NULL;
        gtk_tree_model_get(model, &iter, OBJECT_PANEL_COL_NAME, &name, -1);
        gboolean match = name && strcmp(name, LISTENER_NAME) == 0;
        g_free(name);
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

/* Activates (double-click equivalent) the listener's one connection
 * child row - direct signal invocation, same "trigger the real handler
 * programmatically" style gtk_button_clicked()/gtk_dialog_response()
 * already use elsewhere in this project's tests. */
static gboolean activate_connection_row(GtkWidget *tree_view) {
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));
    GtkTreeIter listener_iter;
    if (!find_listener_row(model, &listener_iter)) {
        return FALSE;
    }
    GtkTreeIter child;
    if (!gtk_tree_model_iter_children(model, &child, &listener_iter)) {
        return FALSE;
    }
    GtkTreePath *path = gtk_tree_model_get_path(model, &child);
    GtkTreeViewColumn *column = gtk_tree_view_get_column(GTK_TREE_VIEW(tree_view), 0);
    gtk_tree_view_row_activated(GTK_TREE_VIEW(tree_view), path, column);
    gtk_tree_path_free(path);
    return TRUE;
}

static int connection_pages(GtkWidget *notebook, GtkWidget **out, int max_out) {
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    int count = 0;
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
        if (tab && tab->type == TAB_TYPE_CONNECTION_TERMINAL) {
            if (out && count < max_out) {
                out[count] = page;
            }
            count++;
        }
    }
    return count;
}

static gboolean label_text_is(GtkWidget *page, const char *key, const char *expected) {
    GtkWidget *label = g_object_get_data(G_OBJECT(page), key);
    if (!label) {
        return FALSE;
    }
    const char *text = gtk_label_get_text(GTK_LABEL(label));
    return text && strcmp(text, expected) == 0;
}

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "connection_terminal_smoke: %s\n", msg);
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

static int connect_client_socket(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LISTENER_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static size_t connection_history_len(TestState *test) {
    ListenerSystem *system = app_get_listener_system(test->app);
    const Connection *connection = object_registry_get_connection(system->registry, test->connection_id);
    return connection ? terminal_history_len(connection->history) : 0;
}

static gboolean history_contains(TestState *test, const char *needle) {
    ListenerSystem *system = app_get_listener_system(test->app);
    const Connection *connection = object_registry_get_connection(system->registry, test->connection_id);
    if (!connection) {
        return FALSE;
    }
    size_t len = terminal_history_len(connection->history);
    char buf[256];
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    terminal_history_read(connection->history, 0, buf, len);
    buf[len] = '\0';
    return strstr(buf, needle) != NULL;
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

        switch (test->step) {
            case STEP_OPEN_DIALOG: {
                GtkWidget *new_button = find_by_data_key(GTK_WIDGET(window), "toolbox-new-listener-button");
                if (!new_button) {
                    break; /* window may not have finished its initial build yet */
                }
                gtk_button_clicked(GTK_BUTTON(new_button));

                GtkWidget *dialog = find_new_listener_dialog();
                if (!dialog) {
                    fail(test, "New Listener dialog did not appear");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *name_entry = find_by_data_key(dialog, "toolbox-listener-name-entry");
                GtkWidget *callback_entry = find_by_data_key(dialog, "toolbox-listener-callback-host-entry");
                if (!name_entry || !callback_entry) {
                    fail(test, "could not find dialog fields");
                    return G_SOURCE_REMOVE;
                }
                gtk_entry_set_text(GTK_ENTRY(name_entry), LISTENER_NAME);
                gtk_entry_set_text(GTK_ENTRY(callback_entry), "127.0.0.1");
                gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

                test->step = STEP_WAIT_LISTENER_RUNNING;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_LISTENER_RUNNING: {
                GtkTreeModel *model = tree_view ? gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view)) : NULL;
                GtkTreeIter iter;
                if (model && find_listener_row(model, &iter) && row_state_is(model, &iter, "RUNNING")) {
                    test->step = STEP_OPEN_CLIENT_SOCKET;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_OPEN_CLIENT_SOCKET: {
                test->client_fd = connect_client_socket();
                if (test->client_fd < 0) {
                    fail(test, "client socket failed to connect");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_CONNECTION_ROW;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_CONNECTION_ROW: {
                GtkTreeModel *model = tree_view ? gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view)) : NULL;
                GtkTreeIter listener_iter;
                if (model && find_listener_row(model, &listener_iter)) {
                    GtkTreeIter child;
                    if (gtk_tree_model_iter_children(model, &child, &listener_iter) &&
                        row_state_is(model, &child, "CONNECTED")) {
                        guint64 id = 0;
                        gtk_tree_model_get(model, &child, OBJECT_PANEL_COL_ID, &id, -1);
                        test->connection_id = id;
                        test->step = STEP_ACTIVATE_ROW_FIRST_OPEN;
                        test->step_elapsed_ms = 0;
                        continue;
                    }
                }
                break;
            }

            case STEP_ACTIVATE_ROW_FIRST_OPEN: {
                if (!activate_connection_row(tree_view)) {
                    fail(test, "could not activate the connection row");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_PAGE_CONNECTED;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_PAGE_CONNECTED: {
                GtkWidget *pages[4];
                int n = connection_pages(notebook, pages, 4);
                if (n == 1 && label_text_is(pages[0], "toolbox-connection-state-label", "State: CONNECTED") &&
                    label_text_is(pages[0], "toolbox-connection-control-label", "You have control")) {
                    test->step = STEP_SEND_FIRST_DATA;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_SEND_FIRST_DATA: {
                const char msg[] = "hello from remote\n";
                if (send(test->client_fd, msg, strlen(msg), 0) < 0) {
                    fail(test, "failed to send first data from client socket");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_HISTORY_GREW;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_HISTORY_GREW: {
                if (history_contains(test, "hello from remote")) {
                    test->step = STEP_CLOSE_TAB;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_CLOSE_TAB: {
                GtkWidget *pages[4];
                int n = connection_pages(notebook, pages, 4);
                if (n != 1) {
                    fail(test, "expected exactly one connection-terminal page before closing it");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *tab_label = gtk_notebook_get_tab_label(GTK_NOTEBOOK(notebook), pages[0]);
                GPtrArray *buttons = g_ptr_array_new();
                collect_by_type(tab_label, buttons, GTK_TYPE_BUTTON);
                if (buttons->len == 0) {
                    g_ptr_array_free(buttons, TRUE);
                    fail(test, "could not find the tab's close button");
                    return G_SOURCE_REMOVE;
                }
                gtk_button_clicked(GTK_BUTTON(g_ptr_array_index(buttons, 0)));
                g_ptr_array_free(buttons, TRUE);

                test->step = STEP_VERIFY_STILL_CONNECTED;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_VERIFY_STILL_CONNECTED: {
                if (connection_pages(notebook, NULL, 0) != 0) {
                    fail(test, "connection-terminal page should be gone after closing its tab");
                    return G_SOURCE_REMOVE;
                }
                ListenerSystem *system = app_get_listener_system(test->app);
                const Connection *connection = object_registry_get_connection(system->registry, test->connection_id);
                if (!connection || connection->state != CONNECTION_STATE_CONNECTED) {
                    fail(test, "closing the tab should not have disconnected the connection");
                    return G_SOURCE_REMOVE;
                }
                test->history_before_reopen = connection_history_len(test);
                test->step = STEP_REOPEN_ROW;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_REOPEN_ROW: {
                if (!activate_connection_row(tree_view)) {
                    fail(test, "could not re-activate the connection row");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_REOPENED_PAGE;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_REOPENED_PAGE: {
                GtkWidget *pages[4];
                int n = connection_pages(notebook, pages, 4);
                if (n == 1 && label_text_is(pages[0], "toolbox-connection-state-label", "State: CONNECTED")) {
                    test->step = STEP_SEND_SECOND_DATA;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_SEND_SECOND_DATA: {
                const char msg[] = "more remote output\n";
                if (send(test->client_fd, msg, strlen(msg), 0) < 0) {
                    fail(test, "failed to send second data from client socket");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_HISTORY_GREW_MORE;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_HISTORY_GREW_MORE: {
                if (connection_history_len(test) > test->history_before_reopen) {
                    test->step = STEP_OPEN_ANOTHER_VIEW;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_OPEN_ANOTHER_VIEW: {
                GtkWidget *pages[4];
                int n = connection_pages(notebook, pages, 4);
                if (n != 1) {
                    fail(test, "expected exactly one connection-terminal page before opening another");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *open_another = g_object_get_data(G_OBJECT(pages[0]), "toolbox-connection-open-another-button");
                if (!open_another) {
                    fail(test, "could not find the Open Another View button");
                    return G_SOURCE_REMOVE;
                }
                gtk_button_clicked(GTK_BUTTON(open_another));
                test->step = STEP_WAIT_TWO_PAGES;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_TWO_PAGES: {
                GtkWidget *pages[4];
                int n = connection_pages(notebook, pages, 4);
                if (n == 2 && label_text_is(pages[0], "toolbox-connection-control-label", "You have control") &&
                    label_text_is(pages[1], "toolbox-connection-control-label", "Read-only")) {
                    test->step = STEP_TAKE_CONTROL_SECOND;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_TAKE_CONTROL_SECOND: {
                GtkWidget *pages[4];
                connection_pages(notebook, pages, 4);
                GtkWidget *take_control = g_object_get_data(G_OBJECT(pages[1]), "toolbox-connection-take-control-button");
                if (!take_control || !gtk_widget_get_sensitive(take_control)) {
                    fail(test, "expected the second page's Take Control button to be sensitive");
                    return G_SOURCE_REMOVE;
                }
                gtk_button_clicked(GTK_BUTTON(take_control));
                test->step = STEP_WAIT_CONTROL_SWAPPED;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_CONTROL_SWAPPED: {
                GtkWidget *pages[4];
                int n = connection_pages(notebook, pages, 4);
                if (n == 2 && label_text_is(pages[0], "toolbox-connection-control-label", "Read-only") &&
                    label_text_is(pages[1], "toolbox-connection-control-label", "You have control")) {
                    test->history_before_disconnect = connection_history_len(test);
                    test->step = STEP_DISCONNECT_CLIENT;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_DISCONNECT_CLIENT: {
                close(test->client_fd);
                test->client_fd = -1;
                test->step = STEP_WAIT_ROW_DISCONNECTED;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_ROW_DISCONNECTED: {
                GtkTreeModel *model = tree_view ? gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view)) : NULL;
                GtkTreeIter listener_iter;
                if (model && find_listener_row(model, &listener_iter)) {
                    GtkTreeIter child;
                    if (gtk_tree_model_iter_children(model, &child, &listener_iter) &&
                        row_state_is(model, &child, "DISCONNECTED")) {
                        test->step = STEP_ACTIVATE_ROW_AGAIN;
                        test->step_elapsed_ms = 0;
                        continue;
                    }
                }
                break;
            }

            case STEP_ACTIVATE_ROW_AGAIN: {
                if (!activate_connection_row(tree_view)) {
                    fail(test, "could not activate the disconnected connection row");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_VERIFY_DISCONNECTED_READONLY;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_VERIFY_DISCONNECTED_READONLY: {
                GtkWidget *pages[4];
                int n = connection_pages(notebook, pages, 4);
                if (n != 2) {
                    break; /* activation may take a tick to settle */
                }
                gboolean both_disconnected_and_locked = TRUE;
                for (int i = 0; i < n; i++) {
                    if (!label_text_is(pages[i], "toolbox-connection-state-label", "State: DISCONNECTED")) {
                        both_disconnected_and_locked = FALSE;
                    }
                    GtkWidget *take_control = g_object_get_data(G_OBJECT(pages[i]), "toolbox-connection-take-control-button");
                    if (!take_control || gtk_widget_get_sensitive(take_control)) {
                        both_disconnected_and_locked = FALSE;
                    }
                }
                if (!both_disconnected_and_locked) {
                    break;
                }
                if (connection_history_len(test) != test->history_before_disconnect) {
                    fail(test, "history should be unchanged after disconnecting");
                    return G_SOURCE_REMOVE;
                }

                test->done = TRUE;
                gtk_window_close(window);
                return G_SOURCE_REMOVE;
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
        fprintf(stderr, "connection_terminal_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "connection_terminal_smoke: test did not complete\n");
        return 1;
    }

    g_print("connection_terminal_smoke: detach-on-close, reopen-and-resume, single-writer, "
            "and disconnected-read-only all verified\n");
    return 0;
}
