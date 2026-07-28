/*
 * Exercises Phase 6's checkpoint end-to-end in the real app: the New
 * Listener modal opens with no socket, invalid input shows an inline
 * error and never reaches the manager, Callback Host auto-follows Bind
 * Address until manually edited, and a valid submission reaches
 * RUNNING - all through the real GtkDialog, not a headless simulation.
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#include "app/app.h"

#define POLL_INTERVAL_MS 200
#define POLL_TIMEOUT_MS 3000

typedef struct TestState {
    gboolean failed;
    gboolean reached_running;
    int elapsed_ms;
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

/* The dialog is its own top-level, separate from the main window, so
 * gtk_application_get_windows() alone won't surface it - it's tagged
 * with "toolbox-new-listener-dialog" on itself (not a descendant) in
 * open_new_listener_dialog(), so a plain toplevel scan finds it. */
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

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "new_listener_dialog_smoke: %s\n", (msg));    \
            test->failed = TRUE;                                         \
            GtkWindow *w = main_window();                                 \
            if (w) {                                                      \
                gtk_window_close(w);                                      \
            }                                                             \
            return G_SOURCE_REMOVE;                                       \
        }                                                                 \
    } while (0)

static gboolean poll_for_running(gpointer user_data) {
    TestState *test = user_data;
    GtkWindow *window = main_window();
    if (!window) {
        return G_SOURCE_REMOVE;
    }

    GtkWidget *status = find_by_data_key(GTK_WIDGET(window), "toolbox-listener-status-label");
    if (status) {
        const char *text = gtk_label_get_text(GTK_LABEL(status));
        if (text && strstr(text, "RUNNING")) {
            test->reached_running = TRUE;
            gtk_window_close(window);
            return G_SOURCE_REMOVE;
        }
    }

    test->elapsed_ms += POLL_INTERVAL_MS;
    if (test->elapsed_ms >= POLL_TIMEOUT_MS) {
        gtk_window_close(window);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* Everything here runs synchronously within one main-loop callback -
 * gtk_button_clicked()/gtk_entry_set_text()/gtk_dialog_response() all
 * invoke their connected handlers immediately, no extra iterations
 * needed. Only reaching RUNNING (a real worker thread binding
 * asynchronously) needs actual elapsed time, handled by
 * poll_for_running() afterward. */
static gboolean run_scenario(gpointer user_data) {
    TestState *test = user_data;

    GtkWindow *window = main_window();
    CHECK(window != NULL, "main window not found");

    GtkWidget *new_button = find_by_data_key(GTK_WIDGET(window), "toolbox-new-listener-button");
    CHECK(new_button != NULL, "could not find the + New Listener button");
    gtk_button_clicked(GTK_BUTTON(new_button));

    /* Modal opens with no socket: opening it alone must not have created anything. */
    GtkWidget *status = find_by_data_key(GTK_WIDGET(window), "toolbox-listener-status-label");
    CHECK(status != NULL, "could not find the status label");
    CHECK(strcmp(gtk_label_get_text(GTK_LABEL(status)), "No listeners yet") == 0,
          "opening the dialog should not have created anything");

    GtkWidget *dialog = find_new_listener_dialog();
    CHECK(dialog != NULL, "New Listener dialog did not appear");

    GtkWidget *name_entry = find_by_data_key(dialog, "toolbox-listener-name-entry");
    GtkWidget *name_error = find_by_data_key(dialog, "toolbox-listener-name-error");
    GtkWidget *bind_entry = find_by_data_key(dialog, "toolbox-listener-bind-address-entry");
    GtkWidget *port_entry = find_by_data_key(dialog, "toolbox-listener-port-entry");
    GtkWidget *callback_entry = find_by_data_key(dialog, "toolbox-listener-callback-host-entry");
    CHECK(name_entry && name_error && bind_entry && port_entry && callback_entry,
          "could not find every expected dialog field");

    /* Defaults. */
    CHECK(strcmp(gtk_entry_get_text(GTK_ENTRY(port_entry)), "4444") == 0, "port did not default to 4444");
    CHECK(strcmp(gtk_entry_get_text(GTK_ENTRY(bind_entry)), "0.0.0.0") == 0,
          "bind address did not default to 0.0.0.0");

    /* Callback host auto-follows bind address until manually edited. */
    gtk_entry_set_text(GTK_ENTRY(bind_entry), "127.0.0.1");
    CHECK(strcmp(gtk_entry_get_text(GTK_ENTRY(callback_entry)), "127.0.0.1") == 0,
          "callback host did not auto-follow bind address");

    gtk_entry_set_text(GTK_ENTRY(callback_entry), "203.0.113.5");
    gtk_entry_set_text(GTK_ENTRY(bind_entry), "10.0.0.1");
    CHECK(strcmp(gtk_entry_get_text(GTK_ENTRY(callback_entry)), "203.0.113.5") == 0,
          "callback host should stop following once manually edited");

    /* 10.0.0.1 (set above only to prove the dirty flag stops the
     * auto-follow) isn't a real local address in this environment and
     * would fail to bind - swap back to loopback, which always is,
     * before actually submitting below. */
    gtk_entry_set_text(GTK_ENTRY(bind_entry), "127.0.0.1");

    /* Invalid input: empty name shows an inline error and never reaches the manager. */
    gtk_entry_set_text(GTK_ENTRY(name_entry), "");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    CHECK(strlen(gtk_label_get_text(GTK_LABEL(name_error))) > 0, "expected an inline name error for empty input");
    CHECK(strcmp(gtk_label_get_text(GTK_LABEL(status)), "No listeners yet") == 0,
          "invalid submission should not have created anything");
    CHECK(find_new_listener_dialog() == dialog, "dialog should still be open after invalid submission");

    /* Valid submission. */
    gtk_entry_set_text(GTK_ENTRY(name_entry), "SmokeTest");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    CHECK(find_new_listener_dialog() == NULL, "dialog should have closed after a valid submission");

    g_timeout_add(POLL_INTERVAL_MS, poll_for_running, test);
    return G_SOURCE_REMOVE;
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);

    g_timeout_add(150, run_scenario, &test);

    int status = app_run(app);
    app_destroy(app);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "new_listener_dialog_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.reached_running) {
        fprintf(stderr, "new_listener_dialog_smoke: status label never reached RUNNING within %dms\n",
                POLL_TIMEOUT_MS);
        return 1;
    }

    g_print("new_listener_dialog_smoke: modal defaults, inline validation, and successful creation all verified\n");
    return 0;
}
