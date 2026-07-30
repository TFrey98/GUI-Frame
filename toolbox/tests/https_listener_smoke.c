/*
 * Exercises Phase 12's checkpoint end-to-end in the real app: "handshake
 * with a test client; bad cert path -> clean START_FAILED; parity with
 * TCP/HTTP." Same dialog-driving convention as every phase since 6, plus
 * http_listener_smoke.c's raw-request/response pattern - except the
 * "raw socket" is now a real TLS session (SSL_connect/SSL_write/SSL_read
 * against a self-signed cert this test generates itself at startup via
 * OpenSSL's C API, not a committed fixture or an external `openssl` CLI
 * call - dependency-free like every other network test in this suite).
 */
#include <arpa/inet.h>
#include <gtk/gtk.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "app/app.h"

#define STEP_INTERVAL_MS 100
#define STEP_TIMEOUT_MS 3000
#define LISTENER_NAME "HttpsSmoke"
#define LISTENER_PORT 4443
#define BAD_CERT_LISTENER_NAME "HttpsBadCert"
#define BAD_CERT_LISTENER_PORT 4442
#define URL_PATH "/checkin"
#define CLIENT_IO_TIMEOUT_SECONDS 2

enum {
    OBJECT_PANEL_COL_NAME,
    OBJECT_PANEL_COL_ENDPOINT,
    OBJECT_PANEL_COL_STATE,
    OBJECT_PANEL_COL_ID
};

typedef enum Step {
    STEP_GENERATE_CERT,
    STEP_OPEN_DIALOG,
    STEP_WAIT_RUNNING,
    STEP_TLS_CONNECT,
    STEP_SEND_MATCHING_REQUEST,
    STEP_READ_MATCHING_RESPONSE,
    STEP_WAIT_CONNECTION_ROW,
    STEP_SEND_MORE_DATA,
    STEP_WAIT_HISTORY_GREW,
    STEP_CLOSE_CLIENT_SESSION,
    STEP_WAIT_DISCONNECTED,
    STEP_OPEN_BAD_CERT_DIALOG,
    STEP_WAIT_BAD_CERT_ERROR
} Step;

typedef struct TestState {
    Step step;
    int step_elapsed_ms;
    gboolean failed;
    gboolean done;
    App *app;
    int client_fd;
    SSL_CTX *client_ctx;
    SSL *client_ssl;
    uint64_t connection_id;
    char cert_path[256];
    char key_path[256];
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

static void close_client_session(TestState *test) {
    if (test->client_ssl) {
        SSL_shutdown(test->client_ssl);
        SSL_free(test->client_ssl);
        test->client_ssl = NULL;
    }
    if (test->client_fd >= 0) {
        close(test->client_fd);
        test->client_fd = -1;
    }
}

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "https_listener_smoke: %s\n", msg);
    test->failed = TRUE;
    close_client_session(test);
    GtkWindow *w = main_window();
    if (w) {
        gtk_window_close(w);
    }
}

/* Generates a throwaway RSA key + self-signed X509 cert for "localhost",
 * writing PEM files to the two given paths. Not a general-purpose CA -
 * just enough for a client with SSL_VERIFY_NONE to complete a real TLS
 * handshake against, same spirit as this suite's other hand-built
 * network fixtures (a raw HTTP request instead of curl, etc). Returns
 * true on success. */
static gboolean generate_self_signed_cert(const char *cert_path, const char *key_path) {
    gboolean ok = FALSE;
    EVP_PKEY *pkey = EVP_RSA_gen(2048);
    if (!pkey) {
        return FALSE;
    }

    X509 *x509 = X509_new();
    if (!x509) {
        EVP_PKEY_free(pkey);
        return FALSE;
    }

    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 31536000L); /* 1 year - plenty for a single test run */
    X509_set_pubkey(x509, pkey);

    X509_NAME *name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"localhost", -1, -1, 0);
    X509_set_issuer_name(x509, name);

    if (X509_sign(x509, pkey, EVP_sha256()) > 0) {
        FILE *key_file = fopen(key_path, "wb");
        FILE *cert_file = fopen(cert_path, "wb");
        if (key_file && cert_file && PEM_write_PrivateKey(key_file, pkey, NULL, NULL, 0, NULL, NULL) == 1 &&
            PEM_write_X509(cert_file, x509) == 1) {
            ok = TRUE;
        }
        if (key_file) {
            fclose(key_file);
        }
        if (cert_file) {
            fclose(cert_file);
        }
    }

    X509_free(x509);
    EVP_PKEY_free(pkey);
    return ok;
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
    struct timeval tv = {.tv_sec = CLIENT_IO_TIMEOUT_SECONDS, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

static void send_all_tls(SSL *ssl, const char *data) {
    size_t len = strlen(data);
    size_t sent = 0;
    while (sent < len) {
        int n = SSL_write(ssl, data + sent, (int)(len - sent));
        if (n <= 0) {
            return;
        }
        sent += (size_t)n;
    }
}

/* Bounded by the client socket's own SO_RCVTIMEO (set in
 * connect_client_socket) rather than a poll() loop - simpler, and this
 * project's servers rely on the identical assumption (blocking fd +
 * kernel timeout) for their own bounded TLS reads; see https_worker.c's
 * set_socket_timeout(). Returns bytes read (buf NUL-terminated) on
 * success, -1 on timeout/error. */
static ssize_t read_response_tls(SSL *ssl, char *buf, size_t buf_size) {
    int n = SSL_read(ssl, buf, (int)(buf_size - 1));
    if (n <= 0) {
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

/* Drives the New Listener dialog to completion for a given
 * name/port/cert/key combination - shared by both the happy-path
 * listener and the deliberately-broken bad-cert-path one below. */
static gboolean submit_https_listener(GtkWindow *window, const char *name, uint16_t port, const char *cert_path,
                                       const char *key_path, TestState *test) {
    GtkWidget *new_button = find_by_data_key(GTK_WIDGET(window), "toolbox-new-listener-button");
    if (!new_button) {
        return FALSE; /* window may not have finished its initial build yet */
    }
    gtk_button_clicked(GTK_BUTTON(new_button));

    GtkWidget *dialog = find_new_listener_dialog();
    if (!dialog) {
        fail(test, "New Listener dialog did not appear");
        return TRUE;
    }
    GtkWidget *name_entry = find_by_data_key(dialog, "toolbox-listener-name-entry");
    GtkWidget *type_combo = find_by_data_key(dialog, "toolbox-listener-type-combo");
    GtkWidget *port_entry = find_by_data_key(dialog, "toolbox-listener-port-entry");
    GtkWidget *callback_entry = find_by_data_key(dialog, "toolbox-listener-callback-host-entry");
    GtkWidget *url_path_entry = find_by_data_key(dialog, "toolbox-listener-url-path-entry");
    GtkWidget *cert_path_entry = find_by_data_key(dialog, "toolbox-listener-cert-path-entry");
    GtkWidget *key_path_entry = find_by_data_key(dialog, "toolbox-listener-key-path-entry");
    if (!name_entry || !type_combo || !port_entry || !callback_entry || !url_path_entry || !cert_path_entry ||
        !key_path_entry) {
        fail(test, "could not find dialog fields");
        return TRUE;
    }
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    gtk_entry_set_text(GTK_ENTRY(name_entry), name);
    gtk_combo_box_set_active(GTK_COMBO_BOX(type_combo), 2); /* HTTPS */
    gtk_entry_set_text(GTK_ENTRY(port_entry), port_text);
    gtk_entry_set_text(GTK_ENTRY(callback_entry), "127.0.0.1");
    gtk_entry_set_text(GTK_ENTRY(url_path_entry), URL_PATH);
    gtk_entry_set_text(GTK_ENTRY(cert_path_entry), cert_path);
    gtk_entry_set_text(GTK_ENTRY(key_path_entry), key_path);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    return TRUE;
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
            case STEP_GENERATE_CERT: {
                snprintf(test->cert_path, sizeof(test->cert_path), "/tmp/toolbox_https_smoke_cert_%d.pem",
                          (int)getpid());
                snprintf(test->key_path, sizeof(test->key_path), "/tmp/toolbox_https_smoke_key_%d.pem",
                          (int)getpid());
                if (!generate_self_signed_cert(test->cert_path, test->key_path)) {
                    fail(test, "failed to generate a self-signed test certificate");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_OPEN_DIALOG;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_OPEN_DIALOG: {
                if (!submit_https_listener(window, LISTENER_NAME, LISTENER_PORT, test->cert_path, test->key_path,
                                            test)) {
                    break;
                }
                if (test->failed) {
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_RUNNING;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_RUNNING: {
                GtkTreeIter iter;
                if (model && find_row_by_name(model, LISTENER_NAME, &iter) && row_state_is(model, &iter, "RUNNING")) {
                    test->step = STEP_TLS_CONNECT;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_TLS_CONNECT: {
                test->client_fd = connect_client_socket(LISTENER_PORT);
                if (test->client_fd < 0) {
                    fail(test, "client socket failed to connect");
                    return G_SOURCE_REMOVE;
                }
                test->client_ssl = SSL_new(test->client_ctx);
                SSL_set_fd(test->client_ssl, test->client_fd);
                if (SSL_connect(test->client_ssl) != 1) {
                    fail(test, "TLS handshake failed");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_SEND_MATCHING_REQUEST;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_SEND_MATCHING_REQUEST: {
                send_all_tls(test->client_ssl, "GET " URL_PATH " HTTP/1.1\r\nHost: x\r\n\r\n");
                test->step = STEP_READ_MATCHING_RESPONSE;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_READ_MATCHING_RESPONSE: {
                ssize_t n = read_response_tls(test->client_ssl, response, sizeof(response));
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
                send_all_tls(test->client_ssl, "post-handshake traffic\n");
                test->step = STEP_WAIT_HISTORY_GREW;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_HISTORY_GREW: {
                if (connection_history_len(test, test->connection_id) > 0) {
                    test->step = STEP_CLOSE_CLIENT_SESSION;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_CLOSE_CLIENT_SESSION: {
                close_client_session(test);
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
                        test->step = STEP_OPEN_BAD_CERT_DIALOG;
                        test->step_elapsed_ms = 0;
                        continue;
                    }
                }
                break;
            }

            case STEP_OPEN_BAD_CERT_DIALOG: {
                if (!submit_https_listener(window, BAD_CERT_LISTENER_NAME, BAD_CERT_LISTENER_PORT,
                                            "/nonexistent/path/does-not-exist.pem", test->key_path, test)) {
                    break;
                }
                if (test->failed) {
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_BAD_CERT_ERROR;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_BAD_CERT_ERROR: {
                GtkTreeIter iter;
                if (model && find_row_by_name(model, BAD_CERT_LISTENER_NAME, &iter)) {
                    if (row_state_is(model, &iter, "RUNNING")) {
                        fail(test, "a bad certificate path should never reach RUNNING");
                        return G_SOURCE_REMOVE;
                    }
                    if (row_state_is(model, &iter, "ERROR")) {
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
    test.client_ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(test.client_ctx, SSL_VERIFY_NONE, NULL); /* trusting a self-signed test cert is expected here */
    test.app = app_create(0, NULL);

    g_timeout_add(STEP_INTERVAL_MS, drive, &test);

    int status = app_run(test.app);
    app_destroy(test.app);

    close_client_session(&test);
    SSL_CTX_free(test.client_ctx);
    if (test.cert_path[0]) {
        unlink(test.cert_path);
    }
    if (test.key_path[0]) {
        unlink(test.key_path);
    }

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "https_listener_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "https_listener_smoke: test did not complete\n");
        return 1;
    }

    g_print("https_listener_smoke: TLS handshake, matching path, identical connection lifecycle, and "
            "bad-cert-path START_FAILED all verified\n");
    return 0;
}
