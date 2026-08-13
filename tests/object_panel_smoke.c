/*
 * Exercises Phase 8's checkpoint end-to-end in the real app: "connections
 * appear under their listener as they connect and remain when
 * disconnected." Combines the dialog-driving approach every GTK smoke
 * test since Phase 6 uses with the raw client-socket approach
 * connection_io_test.c uses to trigger a real CONNECTION_OPENED/CLOSED -
 * this is the first GTK smoke test to also open a real client socket.
 */
#include <arpa/inet.h>
#include <gtk/gtk.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "app/app.h"
#include "test_gtk_utils.h"

#define STEP_INTERVAL_MS 100
#define STEP_TIMEOUT_MS 3000
#define LISTENER_NAME "PanelSmoke"
#define LISTENER_PORT 4444

/* Mirrors ui_gtk.c's file-local OBJECT_PANEL_COL_* enum - not a public
 * API, same convention listener_console_tab_smoke.c already uses for
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
    STEP_CLOSE_CLIENT_SOCKET,
    STEP_WAIT_DISCONNECTED
} Step;

typedef struct TestState {
    Step step;
    int step_elapsed_ms;
    gboolean failed;
    gboolean done;
    int client_fd;
    int initial_child_count;
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
        if (g_object_get_data(G_OBJECT(w), "workbench-new-listener-dialog")) {
            found = w;
            break;
        }
    }
    g_list_free(toplevels);
    return found;
}

static GtkTreeModel *object_panel_model(GtkWidget *window) {
    GtkWidget *tree_view = find_by_data_key(window, "workbench-object-panel-tree");
    if (!tree_view) {
        return NULL;
    }
    return gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));
}

/* Registry order is insertion order and this test only ever creates one
 * listener, so matching by name (rather than id, which this black-box
 * test has no easy way to learn) is enough to identify the row. */
static gboolean find_listener_row(GtkTreeModel *model, GtkTreeIter *out_iter) {
    if (!model) {
        return FALSE;
    }
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

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "object_panel_smoke: %s\n", msg);
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

static gboolean drive(gpointer user_data) {
    TestState *test = user_data;

    for (;;) {
        GtkWindow *window = main_window();
        if (!window) {
            return G_SOURCE_REMOVE;
        }

        switch (test->step) {
            case STEP_OPEN_DIALOG: {
                GtkWidget *new_button = find_by_data_key(GTK_WIDGET(window), "workbench-new-listener-button");
                if (!new_button) {
                    /* The window may not have finished its initial build on
                     * the very first tick - wait rather than fail immediately. */
                    break;
                }
                gtk_button_clicked(GTK_BUTTON(new_button));

                GtkWidget *dialog = find_new_listener_dialog();
                if (!dialog) {
                    fail(test, "New Listener dialog did not appear");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *name_entry = find_by_data_key(dialog, "workbench-listener-name-entry");
                GtkWidget *callback_entry = find_by_data_key(dialog, "workbench-listener-callback-host-entry");
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
                GtkTreeModel *model = object_panel_model(GTK_WIDGET(window));
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
                GtkTreeModel *model = object_panel_model(GTK_WIDGET(window));
                GtkTreeIter listener_iter;
                if (model && find_listener_row(model, &listener_iter)) {
                    GtkTreeIter child;
                    if (gtk_tree_model_iter_children(model, &child, &listener_iter) &&
                        row_state_is(model, &child, "CONNECTED")) {
                        gchar *endpoint = NULL;
                        gtk_tree_model_get(model, &child, OBJECT_PANEL_COL_ENDPOINT, &endpoint, -1);
                        gboolean has_endpoint = endpoint && strlen(endpoint) > 0;
                        g_free(endpoint);
                        if (!has_endpoint) {
                            fail(test, "connection row has no endpoint text");
                            return G_SOURCE_REMOVE;
                        }
                        test->initial_child_count = gtk_tree_model_iter_n_children(model, &listener_iter);
                        test->step = STEP_CLOSE_CLIENT_SOCKET;
                        test->step_elapsed_ms = 0;
                        continue;
                    }
                }
                break;
            }

            case STEP_CLOSE_CLIENT_SOCKET: {
                close(test->client_fd);
                test->client_fd = -1;
                test->step = STEP_WAIT_DISCONNECTED;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_DISCONNECTED: {
                GtkTreeModel *model = object_panel_model(GTK_WIDGET(window));
                GtkTreeIter listener_iter;
                if (model && find_listener_row(model, &listener_iter)) {
                    GtkTreeIter child;
                    if (gtk_tree_model_iter_children(model, &child, &listener_iter) &&
                        row_state_is(model, &child, "DISCONNECTED")) {
                        int child_count = gtk_tree_model_iter_n_children(model, &listener_iter);
                        if (child_count != test->initial_child_count) {
                            fail(test, "connection row disappeared instead of just changing state");
                            return G_SOURCE_REMOVE;
                        }
                        test->done = TRUE;
                        gtk_window_close(window);
                        return G_SOURCE_REMOVE;
                    }
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
    App *app = app_create(0, NULL);

    g_timeout_add(STEP_INTERVAL_MS, drive, &test);

    int status = app_run(app);
    app_destroy(app);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "object_panel_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "object_panel_smoke: test did not complete\n");
        return 1;
    }

    g_print("object_panel_smoke: listener row, live connection row, and disconnect-but-retain all verified\n");
    return 0;
}
