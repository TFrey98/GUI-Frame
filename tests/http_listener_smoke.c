/*
 * Exercises Phase 11's checkpoint end-to-end in the real app: "HTTP
 * client hits the path; identical lifecycle." Same dialog-driving
 * convention as every phase since 6, plus the raw-socket pattern from
 * connection_io_test.c/object_panel_smoke.c - a hand-built HTTP request
 * over a raw socket, not curl, keeping this dependency-free like every
 * other network test in this suite.
 */
#include <arpa/inet.h>
#include <gtk/gtk.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "app/app.h"
#include "test_gtk_utils.h"

#define STEP_INTERVAL_MS 100
#define STEP_TIMEOUT_MS 3000
#define LISTENER_NAME "HttpSmoke"
#define LISTENER_PORT 4444
#define URL_PATH "/checkin"

enum {
    OBJECT_PANEL_COL_NAME,
    OBJECT_PANEL_COL_ENDPOINT,
    OBJECT_PANEL_COL_STATE,
    OBJECT_PANEL_COL_ID
};

typedef enum Step {
    STEP_OPEN_DIALOG,
    STEP_WAIT_RUNNING,
    STEP_OPEN_CLIENT_SOCKET,
    STEP_SEND_MATCHING_REQUEST,
    STEP_READ_MATCHING_RESPONSE,
    STEP_WAIT_CONNECTION_ROW,
    STEP_SEND_MORE_DATA,
    STEP_WAIT_HISTORY_GREW,
    STEP_CLOSE_CLIENT_SOCKET,
    STEP_WAIT_DISCONNECTED,
    STEP_OPEN_SECOND_SOCKET,
    STEP_SEND_WRONG_PATH_REQUEST,
    STEP_READ_WRONG_PATH_RESPONSE,
    STEP_VERIFY_NO_NEW_CONNECTION
} Step;

typedef struct TestState {
    Step step;
    int step_elapsed_ms;
    gboolean failed;
    gboolean done;
    App *app;
    int client_fd;
    uint64_t connection_id;
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

static GtkWidget *object_panel_tree(GtkWidget *window) {
    return find_by_data_key(window, "workbench-object-panel-tree");
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

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "http_listener_smoke: %s\n", msg);
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

static void send_all(int fd, const char *data) {
    size_t len = strlen(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n <= 0) {
            return;
        }
        sent += (size_t)n;
    }
}

/* Non-blocking-ish bounded read via poll() - the response is small and
 * arrives in one write() on the server side, so a short timeout is
 * plenty; returns bytes read (buf NUL-terminated), or -1 on timeout. */
static ssize_t read_response(int fd, char *buf, size_t buf_size) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    int ready = poll(&pfd, 1, 2000);
    if (ready <= 0) {
        return -1;
    }
    ssize_t n = read(fd, buf, buf_size - 1);
    if (n < 0) {
        return -1;
    }
    buf[n] = '\0';
    return n;
}

static size_t connection_history_len(TestState *test, uint64_t connection_id) {
    ListenerSystem *system = app_get_listener_system(test->app);
    const Connection *connection = object_registry_get_connection(system->registry, connection_id);
    return connection ? terminal_history_len(connection->history) : 0;
}

static gboolean drive(gpointer user_data) {
    TestState *test = user_data;
    static char response[512];

    for (;;) {
        GtkWindow *window = main_window();
        if (!window) {
            return G_SOURCE_REMOVE;
        }
        GtkWidget *tree_view = object_panel_tree(GTK_WIDGET(window));
        GtkTreeModel *model = tree_view ? gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view)) : NULL;

        switch (test->step) {
            case STEP_OPEN_DIALOG: {
                GtkWidget *new_button = find_by_data_key(GTK_WIDGET(window), "workbench-new-listener-button");
                if (!new_button) {
                    break; /* window may not have finished its initial build yet */
                }
                gtk_button_clicked(GTK_BUTTON(new_button));

                GtkWidget *dialog = find_new_listener_dialog();
                if (!dialog) {
                    fail(test, "New Listener dialog did not appear");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *name_entry = find_by_data_key(dialog, "workbench-listener-name-entry");
                GtkWidget *type_combo = find_by_data_key(dialog, "workbench-listener-type-combo");
                GtkWidget *callback_entry = find_by_data_key(dialog, "workbench-listener-callback-host-entry");
                GtkWidget *url_path_entry = find_by_data_key(dialog, "workbench-listener-url-path-entry");
                if (!name_entry || !type_combo || !callback_entry || !url_path_entry) {
                    fail(test, "could not find dialog fields");
                    return G_SOURCE_REMOVE;
                }
                gtk_entry_set_text(GTK_ENTRY(name_entry), LISTENER_NAME);
                gtk_combo_box_set_active(GTK_COMBO_BOX(type_combo), 1); /* HTTP */
                gtk_entry_set_text(GTK_ENTRY(callback_entry), "127.0.0.1");
                gtk_entry_set_text(GTK_ENTRY(url_path_entry), URL_PATH);
                gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

                test->step = STEP_WAIT_RUNNING;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_RUNNING: {
                GtkTreeIter iter;
                if (model && find_row_by_name(model, LISTENER_NAME, &iter) && row_state_is(model, &iter, "RUNNING")) {
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
                test->step = STEP_SEND_MATCHING_REQUEST;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_SEND_MATCHING_REQUEST: {
                send_all(test->client_fd, "GET " URL_PATH " HTTP/1.1\r\nHost: x\r\n\r\n");
                test->step = STEP_READ_MATCHING_RESPONSE;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_READ_MATCHING_RESPONSE: {
                ssize_t n = read_response(test->client_fd, response, sizeof(response));
                if (n < 0) {
                    break; /* response may still be in flight - keep polling until the timeout */
                }
                if (strncmp(response, "HTTP/1.1 200", 12) != 0) {
                    fail(test, "expected a 200 response for the matching path");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_CONNECTION_ROW;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_CONNECTION_ROW: {
                GtkTreeIter listener_iter;
                if (model && find_row_by_name(model, LISTENER_NAME, &listener_iter)) {
                    GtkTreeIter child;
                    if (gtk_tree_model_iter_children(model, &child, &listener_iter) &&
                        row_state_is(model, &child, "CONNECTED")) {
                        test->connection_id = row_id(model, &child);
                        test->step = STEP_SEND_MORE_DATA;
                        test->step_elapsed_ms = 0;
                        continue;
                    }
                }
                break;
            }

            case STEP_SEND_MORE_DATA: {
                send_all(test->client_fd, "post-handshake traffic\n");
                test->step = STEP_WAIT_HISTORY_GREW;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_HISTORY_GREW: {
                if (connection_history_len(test, test->connection_id) > 0) {
                    test->step = STEP_CLOSE_CLIENT_SOCKET;
                    test->step_elapsed_ms = 0;
                    continue;
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
                GtkTreeIter listener_iter;
                if (model && find_row_by_name(model, LISTENER_NAME, &listener_iter)) {
                    GtkTreeIter child;
                    if (gtk_tree_model_iter_children(model, &child, &listener_iter) &&
                        row_state_is(model, &child, "DISCONNECTED")) {
                        test->step = STEP_OPEN_SECOND_SOCKET;
                        test->step_elapsed_ms = 0;
                        continue;
                    }
                }
                break;
            }

            case STEP_OPEN_SECOND_SOCKET: {
                test->client_fd = connect_client_socket();
                if (test->client_fd < 0) {
                    fail(test, "second client socket failed to connect");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_SEND_WRONG_PATH_REQUEST;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_SEND_WRONG_PATH_REQUEST: {
                send_all(test->client_fd, "GET /wrong-path HTTP/1.1\r\nHost: x\r\n\r\n");
                test->step = STEP_READ_WRONG_PATH_RESPONSE;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_READ_WRONG_PATH_RESPONSE: {
                ssize_t n = read_response(test->client_fd, response, sizeof(response));
                if (n < 0) {
                    break;
                }
                if (strncmp(response, "HTTP/1.1 404", 12) != 0) {
                    fail(test, "expected a 404 response for a non-matching path");
                    return G_SOURCE_REMOVE;
                }
                close(test->client_fd);
                test->client_fd = -1;
                test->step = STEP_VERIFY_NO_NEW_CONNECTION;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_VERIFY_NO_NEW_CONNECTION: {
                GtkTreeIter listener_iter;
                if (!find_row_by_name(model, LISTENER_NAME, &listener_iter)) {
                    fail(test, "listener row disappeared");
                    return G_SOURCE_REMOVE;
                }
                int child_count = gtk_tree_model_iter_n_children(model, &listener_iter);
                if (child_count != 1) {
                    fail(test, "a non-matching path should never have become a connection");
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
        fprintf(stderr, "http_listener_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "http_listener_smoke: test did not complete\n");
        return 1;
    }

    g_print("http_listener_smoke: matching path, identical connection lifecycle, and "
            "non-matching path rejection all verified\n");
    return 0;
}
