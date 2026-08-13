/*
 * Exercises Phase 7's checkpoint end-to-end in the real app: a
 * listener's console tab opens on its own once bound (never at
 * creation time), Stop/Restart both work (including the
 * stop-then-auto-start path from RUNNING), and closing the tab leaves
 * the listener running - invariant 4.
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#include "app/app.h"
#include "core/tab.h"
#include "test_gtk_utils.h"

#define STEP_INTERVAL_MS 100
#define STEP_TIMEOUT_MS 3000

typedef enum Step {
    STEP_OPEN_DIALOG,
    STEP_WAIT_RUNNING_INITIAL,
    STEP_CLICK_STOP,
    STEP_WAIT_STOPPED,
    STEP_CLICK_RESTART_FROM_STOPPED,
    STEP_WAIT_RUNNING_AFTER_RESTART_1,
    STEP_CLICK_RESTART_FROM_RUNNING,
    STEP_WAIT_RUNNING_AFTER_RESTART_2,
    STEP_CLOSE_TAB,
    STEP_VERIFY_STILL_RUNNING
} Step;

typedef struct TestState {
    Step step;
    int step_elapsed_ms;
    gboolean failed;
    gboolean done;
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

/* Same convention as new_listener_dialog_smoke.c - the dialog is its
 * own top-level, tagged on itself. */
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

static GtkWidget *find_notebook(GtkWidget *window) {
    GPtrArray *found = g_ptr_array_new();
    collect_by_type(window, found, GTK_TYPE_NOTEBOOK);
    GtkWidget *notebook = found->len > 0 ? GTK_WIDGET(g_ptr_array_index(found, 0)) : NULL;
    g_ptr_array_free(found, TRUE);
    return notebook;
}

/* Finds the (at most one, in this test) TAB_TYPE_LISTENER page - each
 * page carries its owning Tab* as "workbench-tab", the same convention
 * every page in ui_gtk.c already uses. */
static GtkWidget *find_listener_page(GtkWidget *notebook) {
    if (!notebook) {
        return NULL;
    }
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
        if (tab && tab->type == TAB_TYPE_LISTENER) {
            return page;
        }
    }
    return NULL;
}

/* build_listener_page tags these directly on the page as a lookup
 * table (key -> child widget pointer, since more than one listener
 * page can exist at once), not as self-tags - g_object_get_data(page,
 * key) IS the answer, not a widget to recurse into like the dialog's
 * self-tagged fields. */
static GtkWidget *listener_page_widget(GtkWidget *page, const char *key) {
    return g_object_get_data(G_OBJECT(page), key);
}

static gboolean state_label_contains(GtkWidget *page, const char *needle) {
    GtkWidget *label = listener_page_widget(page, "workbench-listener-state-label");
    if (!label) {
        return FALSE;
    }
    const char *text = gtk_label_get_text(GTK_LABEL(label));
    return text && strstr(text, needle) != NULL;
}

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "listener_console_tab_smoke: %s\n", msg);
    test->failed = TRUE;
    GtkWindow *w = main_window();
    if (w) {
        gtk_window_close(w);
    }
}

/* Runs as many steps as are immediately ready within a single tick
 * (every click/entry-set/response call here is synchronous - see
 * new_listener_dialog_smoke.c's comment on the same point), only
 * actually waiting for the next tick when the current step is polling
 * for something a background worker thread hasn't reported yet. */
static gboolean drive(gpointer user_data) {
    TestState *test = user_data;

    for (;;) {
        GtkWindow *window = main_window();
        if (!window) {
            return G_SOURCE_REMOVE;
        }
        GtkWidget *notebook = find_notebook(GTK_WIDGET(window));

        switch (test->step) {
            case STEP_OPEN_DIALOG: {
                GtkWidget *new_button = find_by_data_key(GTK_WIDGET(window), "workbench-new-listener-button");
                if (!new_button) {
                    /* The window may not have finished its initial
                     * build on the very first tick - wait rather than
                     * fail immediately, same as every other wait step. */
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
                /* Callback host is blank by default (bind address
                 * defaults to the 0.0.0.0 wildcard, which auto-follow
                 * never propagates) - needs a real value to validate. */
                gtk_entry_set_text(GTK_ENTRY(name_entry), "TabSmoke");
                gtk_entry_set_text(GTK_ENTRY(callback_entry), "127.0.0.1");
                gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

                test->step = STEP_WAIT_RUNNING_INITIAL;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_RUNNING_INITIAL: {
                GtkWidget *page = find_listener_page(notebook);
                if (page && state_label_contains(page, "RUNNING")) {
                    test->step = STEP_CLICK_STOP;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_CLICK_STOP: {
                GtkWidget *page = find_listener_page(notebook);
                GtkWidget *stop_button = page ? listener_page_widget(page, "workbench-listener-stop-button") : NULL;
                if (!stop_button) {
                    fail(test, "could not find the Stop button");
                    return G_SOURCE_REMOVE;
                }
                gtk_button_clicked(GTK_BUTTON(stop_button));
                test->step = STEP_WAIT_STOPPED;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_STOPPED: {
                GtkWidget *page = find_listener_page(notebook);
                if (page && state_label_contains(page, "STOPPED")) {
                    GtkWidget *stop_button = listener_page_widget(page, "workbench-listener-stop-button");
                    GtkWidget *restart_button = listener_page_widget(page, "workbench-listener-restart-button");
                    if (gtk_widget_get_sensitive(stop_button)) {
                        fail(test, "expected Stop to be insensitive once STOPPED");
                        return G_SOURCE_REMOVE;
                    }
                    if (!gtk_widget_get_sensitive(restart_button)) {
                        fail(test, "expected Restart to be sensitive once STOPPED");
                        return G_SOURCE_REMOVE;
                    }
                    test->step = STEP_CLICK_RESTART_FROM_STOPPED;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_CLICK_RESTART_FROM_STOPPED: {
                GtkWidget *page = find_listener_page(notebook);
                GtkWidget *restart_button = page ? listener_page_widget(page, "workbench-listener-restart-button") : NULL;
                if (!restart_button) {
                    fail(test, "could not find the Restart button");
                    return G_SOURCE_REMOVE;
                }
                gtk_button_clicked(GTK_BUTTON(restart_button));
                test->step = STEP_WAIT_RUNNING_AFTER_RESTART_1;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_RUNNING_AFTER_RESTART_1: {
                /* listener_manager_restart() from STOPPED, i.e. behaving like start_async(). */
                GtkWidget *page = find_listener_page(notebook);
                if (page && state_label_contains(page, "RUNNING")) {
                    test->step = STEP_CLICK_RESTART_FROM_RUNNING;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_CLICK_RESTART_FROM_RUNNING: {
                GtkWidget *page = find_listener_page(notebook);
                GtkWidget *restart_button = page ? listener_page_widget(page, "workbench-listener-restart-button") : NULL;
                if (!restart_button) {
                    fail(test, "could not find the Restart button (second time)");
                    return G_SOURCE_REMOVE;
                }
                gtk_button_clicked(GTK_BUTTON(restart_button));
                test->step = STEP_WAIT_RUNNING_AFTER_RESTART_2;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_RUNNING_AFTER_RESTART_2: {
                /* listener_manager_restart() from RUNNING, i.e. the
                 * stop-then-auto-start restart_pending path. */
                GtkWidget *page = find_listener_page(notebook);
                if (page && state_label_contains(page, "RUNNING")) {
                    test->step = STEP_CLOSE_TAB;
                    test->step_elapsed_ms = 0;
                    continue;
                }
                break;
            }

            case STEP_CLOSE_TAB: {
                GtkWidget *page = find_listener_page(notebook);
                if (!page) {
                    fail(test, "listener page disappeared before it could be closed");
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

                test->step = STEP_VERIFY_STILL_RUNNING;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_VERIFY_STILL_RUNNING: {
                if (find_listener_page(notebook) != NULL) {
                    fail(test, "listener tab should be gone after closing it");
                    return G_SOURCE_REMOVE;
                }
                /* The core invariant: closing the tab must not have
                 * touched the listener itself. */
                GtkWidget *status = find_by_data_key(GTK_WIDGET(window), "workbench-listener-status-label");
                const char *text = status ? gtk_label_get_text(GTK_LABEL(status)) : NULL;
                if (!text || strcmp(text, "TabSmoke: RUNNING") != 0) {
                    fail(test, "expected the listener to still show RUNNING after its tab was closed");
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
    App *app = app_create(0, NULL);

    g_timeout_add(STEP_INTERVAL_MS, drive, &test);

    int status = app_run(app);
    app_destroy(app);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "listener_console_tab_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "listener_console_tab_smoke: test did not complete\n");
        return 1;
    }

    g_print("listener_console_tab_smoke: tab auto-open, Stop/Restart, and close-leaves-it-running all verified\n");
    return 0;
}
