/*
 * Exercises terminal capture -> Export Database -> Clear Database end to
 * end in the real app: a command typed into the seeded terminal tab shows
 * up as a captured "input" row (via the real tick-driven
 * terminal_pump_pty_output() path, not the lower-level API connection_io_test.c
 * / pty_worker_test.c already cover directly); Export Database writes a
 * JSON file containing it; Clear Database empties the table only after
 * confirming (Cancel leaves rows intact).
 */
#define _GNU_SOURCE /* memmem() */

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app/app.h"
#include "db/database.h"
#include "terminal/terminal.h"
#include "test_gtk_utils.h"

#define STEP_INTERVAL_MS 100
#define STEP_TIMEOUT_MS 5000

static const char *MARKER = "capture-marker-9f3";
static const char *RESPONSE_MARKER = "response-marker-7a1";

typedef struct TestState {
    App *app;
    Terminal *view;
    int elapsed_ms;
    int output_baseline;
    gboolean failed;
    gboolean done;
} TestState;

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "database_export_clear_smoke: %s\n", msg);
    test->failed = TRUE;
}

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

static GtkWidget *find_message_dialog(void) {
    GtkWidget *found = NULL;
    GList *toplevels = gtk_window_list_toplevels();
    for (GList *l = toplevels; l; l = l->next) {
        if (GTK_IS_MESSAGE_DIALOG(l->data)) {
            found = GTK_WIDGET(l->data);
            break;
        }
    }
    g_list_free(toplevels);
    return found;
}

static GtkWidget *find_dialog_by_state_key(const char *key) {
    GtkWidget *found = NULL;
    GList *toplevels = gtk_window_list_toplevels();
    for (GList *l = toplevels; l; l = l->next) {
        if (g_object_get_data(G_OBJECT(l->data), key)) {
            found = GTK_WIDGET(l->data);
            break;
        }
    }
    g_list_free(toplevels);
    return found;
}

typedef struct MarkerSearch {
    gboolean found;
} MarkerSearch;

static void look_for_marker(uint64_t id, const char *terminal_kind, uint64_t terminal_id, const char *direction,
                             int64_t captured_at, const void *data, size_t data_len, void *user_data) {
    (void)id;
    (void)terminal_kind;
    (void)terminal_id;
    (void)captured_at;
    MarkerSearch *search = user_data;
    if (strcmp(direction, "input") == 0 && data_len >= strlen(MARKER) &&
        memmem(data, data_len, MARKER, strlen(MARKER)) != NULL) {
        search->found = TRUE;
    }
}

static gboolean marker_was_captured(void) {
    MarkerSearch search = {.found = FALSE};
    database_for_each_terminal_event(look_for_marker, &search);
    return search.found;
}

static void count_row(uint64_t id, const char *terminal_kind, uint64_t terminal_id, const char *direction,
                       int64_t captured_at, const void *data, size_t data_len, void *user_data) {
    (void)id;
    (void)terminal_kind;
    (void)terminal_id;
    (void)direction;
    (void)captured_at;
    (void)data;
    (void)data_len;
    int *count = user_data;
    (*count)++;
}

static int total_event_count(void) {
    int count = 0;
    database_for_each_terminal_event(count_row, &count);
    return count;
}

static void count_output_row(uint64_t id, const char *terminal_kind, uint64_t terminal_id, const char *direction,
                              int64_t captured_at, const void *data, size_t data_len, void *user_data) {
    (void)id;
    (void)terminal_kind;
    (void)terminal_id;
    (void)captured_at;
    (void)data;
    (void)data_len;
    if (strcmp(direction, "output") == 0) {
        int *count = user_data;
        (*count)++;
    }
}

static int output_row_count(void) {
    int count = 0;
    database_for_each_terminal_event(count_output_row, &count);
    return count;
}

typedef struct OutputSearch {
    const char *needle;
    gboolean found;
} OutputSearch;

static void look_for_output(uint64_t id, const char *terminal_kind, uint64_t terminal_id, const char *direction,
                             int64_t captured_at, const void *data, size_t data_len, void *user_data) {
    (void)id;
    (void)terminal_kind;
    (void)terminal_id;
    (void)captured_at;
    OutputSearch *search = user_data;
    if (strcmp(direction, "output") == 0 && data_len >= strlen(search->needle) &&
        memmem(data, data_len, search->needle, strlen(search->needle)) != NULL) {
        search->found = TRUE;
    }
}

static const char *g_response_needle;

static gboolean response_was_captured(void) {
    OutputSearch search = {.needle = g_response_needle, .found = FALSE};
    database_for_each_terminal_event(look_for_output, &search);
    return search.found;
}

static gboolean wait_until(TestState *test, gboolean (*predicate)(void), const char *timeout_message,
                            GSourceFunc next) {
    if (predicate()) {
        g_timeout_add(STEP_INTERVAL_MS, next, test);
        return TRUE;
    }
    test->elapsed_ms += STEP_INTERVAL_MS;
    if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
        fail(test, timeout_message);
        GtkWindow *window = main_window();
        if (window) {
            gtk_widget_destroy(GTK_WIDGET(window));
        }
        return TRUE; /* handled (as a failure) - caller must not also reschedule itself */
    }
    return FALSE;
}

static gboolean phase_verify_cleared(gpointer user_data) {
    TestState *test = user_data;
    int count = total_event_count();
    if (count != 0) {
        fail(test, "Clear Database (confirmed) should empty the table");
        goto done;
    }
    if (!test->failed) {
        test->done = TRUE;
    }
done : {
    GtkWindow *window = main_window();
    if (window) {
        gtk_widget_destroy(GTK_WIDGET(window));
    }
}
    return G_SOURCE_REMOVE;
}

static gboolean phase_confirm_clear(gpointer user_data) {
    TestState *test = user_data;
    GtkWidget *dialog = find_message_dialog();
    if (!dialog) {
        fail(test, "Clear Database should show a confirmation dialog");
        GtkWindow *window = main_window();
        if (window) {
            gtk_widget_destroy(GTK_WIDGET(window));
        }
        return G_SOURCE_REMOVE;
    }
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
    g_timeout_add(STEP_INTERVAL_MS, phase_verify_cleared, test);
    return G_SOURCE_REMOVE;
}

static gboolean phase_cancel_clear_then_confirm(gpointer user_data) {
    TestState *test = user_data;
    GtkWidget *dialog = find_message_dialog();
    if (!dialog) {
        fail(test, "Clear Database should show a confirmation dialog");
        GtkWindow *window = main_window();
        if (window) {
            gtk_widget_destroy(GTK_WIDGET(window));
        }
        return G_SOURCE_REMOVE;
    }
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    if (total_event_count() <= 0) {
        fail(test, "Cancel on Clear Database should leave rows intact");
        GtkWindow *window = main_window();
        if (window) {
            gtk_widget_destroy(GTK_WIDGET(window));
        }
        return G_SOURCE_REMOVE;
    }

    GtkWindow *window = main_window();
    GtkWidget *clear_button = find_by_data_key(GTK_WIDGET(window), "toolbox-clear-database-button");
    if (!clear_button) {
        fail(test, "Clear Database button not found");
        gtk_widget_destroy(GTK_WIDGET(window));
        return G_SOURCE_REMOVE;
    }
    gtk_button_clicked(GTK_BUTTON(clear_button));
    g_timeout_add(STEP_INTERVAL_MS, phase_confirm_clear, test);
    return G_SOURCE_REMOVE;
}

static gboolean phase_verify_export(gpointer user_data) {
    TestState *test = user_data;

    /* phase_export below typed "test_export.json" as the name but
     * selected YAML - the format selection, not the typed extension,
     * must decide the actual file: expect test_export.yaml to exist
     * (with YAML structure) and test_export.json to NOT exist. */
    const char *root = app_get_file_workspace_root(test->app)->canonical_path;
    char wrong_path[4400], export_path[4400];
    snprintf(wrong_path, sizeof(wrong_path), "%s/test_export.json", root);
    snprintf(export_path, sizeof(export_path), "%s/test_export.yaml", root);

    if (access(wrong_path, F_OK) == 0) {
        fail(test, "export should use the .yaml extension for YAML, not whatever was typed");
        goto done;
    }

    FILE *f = fopen(export_path, "rb");
    if (!f) {
        fail(test, "exported YAML file (test_export.yaml) was not written");
        goto done;
    }
    char contents[8192] = {0};
    size_t n = fread(contents, 1, sizeof(contents) - 1, f);
    fclose(f);
    contents[n] = '\0';
    if (contents[0] != '-' || strstr(contents, MARKER) == NULL || strstr(contents, "direction: \"input\"") == NULL) {
        fail(test, "exported YAML did not contain the captured marker row");
        goto done;
    }

    GtkWindow *window = main_window();
    GtkWidget *clear_button = find_by_data_key(GTK_WIDGET(window), "toolbox-clear-database-button");
    if (!clear_button) {
        fail(test, "Clear Database button not found");
        goto done;
    }
    gtk_button_clicked(GTK_BUTTON(clear_button));
    g_timeout_add(STEP_INTERVAL_MS, phase_cancel_clear_then_confirm, test);
    return G_SOURCE_REMOVE;

done : {
    GtkWindow *window = main_window();
    if (window) {
        gtk_widget_destroy(GTK_WIDGET(window));
    }
}
    return G_SOURCE_REMOVE;
}

static gboolean phase_export(gpointer user_data) {
    TestState *test = user_data;
    GtkWindow *window = main_window();
    GtkWidget *export_button = find_by_data_key(GTK_WIDGET(window), "toolbox-export-database-button");
    if (!export_button) {
        fail(test, "Export Database button not found");
        gtk_widget_destroy(GTK_WIDGET(window));
        return G_SOURCE_REMOVE;
    }
    gtk_button_clicked(GTK_BUTTON(export_button));

    GtkWidget *dialog = find_dialog_by_state_key("toolbox-export-database-dialog-state");
    if (!dialog) {
        fail(test, "Export Database dialog did not appear");
        gtk_widget_destroy(GTK_WIDGET(window));
        return G_SOURCE_REMOVE;
    }
    GtkWidget *path_entry = find_by_data_key(dialog, "toolbox-export-database-path-entry");
    GtkWidget *yaml_radio = find_by_data_key(dialog, "toolbox-export-database-yaml-radio");
    if (!path_entry || !yaml_radio) {
        fail(test, "Export Database path entry or YAML radio button not found");
        gtk_widget_destroy(GTK_WIDGET(window));
        return G_SOURCE_REMOVE;
    }
    /* Deliberately mismatched: typed name carries a .json extension, but
     * YAML is selected - the format selection must win. */
    gtk_entry_set_text(GTK_ENTRY(path_entry), "test_export.json");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(yaml_radio), TRUE);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    g_timeout_add(STEP_INTERVAL_MS, phase_verify_export, test);
    return G_SOURCE_REMOVE;
}

/* Waits for a real command's response (sent only after the completing
 * Enter, see phase_verify_no_premature_output) to show up as an "output"
 * row - proving genuine post-Enter output still gets captured once the
 * fast-typing fix stops persisting pre-Enter echo. */
static gboolean phase_wait_for_response(gpointer user_data) {
    TestState *test = user_data;
    g_response_needle = RESPONSE_MARKER;
    if (wait_until(test, response_was_captured, "genuine command output was never captured after Enter",
                    phase_export)) {
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* The actual regression check: a multi-character burst of typed-but-not-
 * yet-submitted text (simulating a fast typist, whose echoed keystrokes
 * can land in a single drain well past 1 byte) must produce zero new
 * "output" rows, since Enter was never pressed. */
static gboolean phase_verify_no_premature_output(gpointer user_data) {
    TestState *test = user_data;
    if (output_row_count() != test->output_baseline) {
        fail(test, "typing without pressing Enter must not add any \"output\" rows (fast-typing echo leaked "
                    "into capture)");
        GtkWindow *window = main_window();
        if (window) {
            gtk_widget_destroy(GTK_WIDGET(window));
        }
        return G_SOURCE_REMOVE;
    }

    /* Complete the line - real command output should now be captured. */
    char rest[128];
    snprintf(rest, sizeof(rest), " %s\n", RESPONSE_MARKER);
    terminal_send(test->view, rest, strlen(rest));

    g_timeout_add(STEP_INTERVAL_MS, phase_wait_for_response, test);
    return G_SOURCE_REMOVE;
}

static gboolean phase_send_partial_fragment(gpointer user_data) {
    TestState *test = user_data;
    test->output_baseline = output_row_count();

    /* No trailing newline - Enter is never pressed here, so this must
     * stay purely echo: multiple characters, well past the old "skip if
     * <=1 byte" heuristic's blind spot, arriving in one drain. */
    static const char partial[] = "echo partial-fragment-no-newline-yet";
    terminal_send(test->view, partial, strlen(partial));

    g_timeout_add(STEP_INTERVAL_MS, phase_verify_no_premature_output, test);
    return G_SOURCE_REMOVE;
}

static gboolean phase_wait_for_capture(gpointer user_data) {
    TestState *test = user_data;
    if (wait_until(test, marker_was_captured, "typed command was never captured as an input event",
                    phase_send_partial_fragment)) {
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean phase_send_command(gpointer user_data) {
    TestState *test = user_data;
    GtkWindow *window = main_window();
    GPtrArray *notebooks = g_ptr_array_new();
    if (window) {
        collect_by_type(GTK_WIDGET(window), notebooks, GTK_TYPE_NOTEBOOK);
    }
    GtkWidget *notebook = notebooks->len > 0 ? GTK_WIDGET(g_ptr_array_index(notebooks, 0)) : NULL;
    g_ptr_array_free(notebooks, TRUE);
    if (!notebook || gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) < 1) {
        fail(test, "seeded terminal tab never appeared");
        if (window) {
            gtk_widget_destroy(GTK_WIDGET(window));
        }
        return G_SOURCE_REMOVE;
    }

    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), 0);
    Terminal *view = g_object_get_data(G_OBJECT(page), "toolbox-view");
    if (!view) {
        fail(test, "seeded terminal tab has no Terminal view attached");
        gtk_widget_destroy(GTK_WIDGET(window));
        return G_SOURCE_REMOVE;
    }
    test->view = view;

    char command[128];
    snprintf(command, sizeof(command), "echo %s\n", MARKER);
    terminal_send(view, command, strlen(command));

    g_timeout_add(STEP_INTERVAL_MS, phase_wait_for_capture, test);
    return G_SOURCE_REMOVE;
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "database_export_clear_smoke: app_create failed\n");
        return 1;
    }
    test.app = app;

    /* This app's toolbox.db is a single file shared by every App-backed
     * test in this suite (all run with the same working directory) -
     * start from a clean slate so this test's row-count assertions don't
     * depend on ctest run order, the same precaution every explorer/editor
     * smoke test already takes for the shared files/ workspace root. Same
     * reasoning for the two export artifacts a previous run of this same
     * test may have left behind. */
    database_clear_terminal_events();
    char stale_json[4400], stale_yaml[4400];
    snprintf(stale_json, sizeof(stale_json), "%s/test_export.json", app_get_file_workspace_root(app)->canonical_path);
    snprintf(stale_yaml, sizeof(stale_yaml), "%s/test_export.yaml", app_get_file_workspace_root(app)->canonical_path);
    unlink(stale_json);
    unlink(stale_yaml);

    g_timeout_add(400, phase_send_command, &test);

    int status = app_run(app);
    app_destroy(app);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "database_export_clear_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "database_export_clear_smoke: test did not complete\n");
        return 1;
    }

    g_print("database_export_clear_smoke: capture-on-type, Export Database (JSON), and Clear Database "
            "(Cancel then confirm) all verified\n");
    return 0;
}
