/*
 * Exercises Step 5's "Run with Arguments..." checkpoint end-to-end in
 * the real app, driving the real dialog: a fixture script that writes
 * its own pwd, argv, and an env-override variable to a sentinel path
 * proves working directory, arguments, and environment overrides all
 * actually reached the real spawned process - not just that a tab
 * appeared. Covers both "create new terminal" (a real argv spawn) and
 * "reuse existing terminal" (typed into the startup terminal's already-
 * live shell), plus that a working directory outside the workspace is
 * rejected inline with nothing spawned.
 */
#define _GNU_SOURCE /* memmem() */

#include <dirent.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/app.h"
#include "core/tab.h"
#include "db/database.h"
#include "files/file_operations.h"
#include "files/workspace_root.h"
#include "test_gtk_utils.h"

enum {
    EXPLORER_COL_ICON,
    EXPLORER_COL_NAME,
    EXPLORER_COL_PATH,
    EXPLORER_COL_IS_DIR,
    EXPLORER_COL_LOADED,
    EXPLORER_COL_SOURCE,
    EXPLORER_COL_NODE_ID
};

#define STEP_INTERVAL_MS 100
#define STEP_TIMEOUT_MS 8000

typedef enum Step {
    STEP_WAIT_UI,
    STEP_SUBMIT_NEW_TERMINAL_RUN,
    STEP_WAIT_NEW_TERMINAL_SENTINEL,
    STEP_WAIT_NEW_TERMINAL_CAPTURE,
    STEP_VALIDATE_REJECTION,
    STEP_SUBMIT_REUSE_RUN,
    STEP_WAIT_REUSE_SENTINEL,
    STEP_WAIT_REUSE_CAPTURE,
} Step;

typedef struct TestState {
    Step step;
    int step_elapsed_ms;
    gboolean failed;
    gboolean done;
    WorkspaceRoot root;
    char sentinel_new[256];
    char sentinel_reuse[256];
    int terminals_before_reuse;
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

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "terminal_run_command_smoke: %s\n", msg);
    test->failed = TRUE;
}

static gboolean row_name_is(GtkTreeModel *model, GtkTreeIter *iter, const char *expected) {
    gchar *name = NULL;
    gtk_tree_model_get(model, iter, EXPLORER_COL_NAME, &name, -1);
    gboolean match = name && strcmp(name, expected) == 0;
    g_free(name);
    return match;
}

static gboolean find_child_by_name(GtkTreeModel *model, GtkTreeIter *parent, const char *name, GtkTreeIter *out) {
    GtkTreeIter child;
    if (!gtk_tree_model_iter_children(model, &child, parent)) {
        return FALSE;
    }
    do {
        if (row_name_is(model, &child, name)) {
            *out = child;
            return TRUE;
        }
    } while (gtk_tree_model_iter_next(model, &child));
    return FALSE;
}

static GtkWidget *open_menu_for_row(GtkWidget *tree_view, GtkTreeModel *model, GtkTreeIter *iter) {
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gtk_tree_view_expand_to_path(GTK_TREE_VIEW(tree_view), path);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view)), path);
    gtk_tree_path_free(path);
    gboolean handled = FALSE;
    g_signal_emit_by_name(tree_view, "popup-menu", &handled);
    return g_object_get_data(G_OBJECT(tree_view), "toolbox-explorer-context-menu");
}

static GtkWidget *find_menu_item(GtkWidget *menu, const char *label) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
    GtkWidget *result = NULL;
    for (GList *l = children; l; l = l->next) {
        GtkWidget *item = GTK_WIDGET(l->data);
        if (!GTK_IS_MENU_ITEM(item)) {
            continue;
        }
        GtkWidget *label_widget = gtk_bin_get_child(GTK_BIN(item));
        if (GTK_IS_LABEL(label_widget) && strcmp(gtk_label_get_text(GTK_LABEL(label_widget)), label) == 0) {
            result = item;
            break;
        }
    }
    g_list_free(children);
    return result;
}

static gboolean click_menu_item(GtkWidget *menu, const char *label) {
    GtkWidget *item = find_menu_item(menu, label);
    if (!item) {
        return FALSE;
    }
    g_signal_emit_by_name(item, "activate");
    gtk_menu_popdown(GTK_MENU(menu));
    return TRUE;
}

static GtkWidget *find_run_dialog(void) {
    GtkWidget *found = NULL;
    GList *toplevels = gtk_window_list_toplevels();
    for (GList *l = toplevels; l; l = l->next) {
        if (g_object_get_data(G_OBJECT(l->data), "toolbox-run-with-arguments-dialog")) {
            found = GTK_WIDGET(l->data);
            break;
        }
    }
    g_list_free(toplevels);
    return found;
}

static int count_terminal_pages(GtkWidget *notebook) {
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    int count = 0;
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
        if (tab && tab->type == TAB_TYPE_TERMINAL) {
            count++;
        }
    }
    return count;
}

static gboolean read_sentinel(const char *path, char *out, size_t out_size) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return FALSE;
    }
    size_t n = fread(out, 1, out_size - 1, f);
    out[n] = '\0';
    fclose(f);
    return n > 0;
}

static void write_file_bytes(const char *path, const char *content, size_t len) {
    FILE *f = fopen(path, "wb");
    if (f) {
        if (len > 0) {
            fwrite(content, 1, len, f);
        }
        fclose(f);
    }
}

typedef struct EventSearch {
    const char *direction;
    const char *needle;
    gboolean found;
} EventSearch;

static void look_for_event(uint64_t id, const char *terminal_kind, uint64_t terminal_id, const char *direction,
                            int64_t captured_at, const void *data, size_t data_len, void *user_data) {
    (void)id;
    (void)terminal_kind;
    (void)terminal_id;
    (void)captured_at;
    EventSearch *search = user_data;
    if (strcmp(direction, search->direction) == 0 && data_len >= strlen(search->needle) &&
        memmem(data, data_len, search->needle, strlen(search->needle)) != NULL) {
        search->found = TRUE;
    }
}

static gboolean event_was_captured(const char *direction, const char *needle) {
    EventSearch search = {.direction = direction, .needle = needle, .found = FALSE};
    database_for_each_terminal_event(look_for_event, &search);
    return search.found;
}

static gboolean drive(gpointer user_data) {
    TestState *test = user_data;

    for (;;) {
        GtkWindow *window = main_window();
        if (!window) {
            return G_SOURCE_REMOVE;
        }
        GtkWidget *tree_view = find_by_data_key(GTK_WIDGET(window), "toolbox-explorer-tree");
        GPtrArray *notebooks = g_ptr_array_new();
        collect_by_type(GTK_WIDGET(window), notebooks, GTK_TYPE_NOTEBOOK);
        GtkWidget *notebook = notebooks->len > 0 ? GTK_WIDGET(g_ptr_array_index(notebooks, 0)) : NULL;
        g_ptr_array_free(notebooks, TRUE);

        switch (test->step) {
            case STEP_WAIT_UI: {
                if (!tree_view || !notebook) {
                    break;
                }
                GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));
                GtkTreeIter toolbox_iter;
                if (!gtk_tree_model_get_iter_first(model, &toolbox_iter) ||
                    !row_name_is(model, &toolbox_iter, "TOOLBOX")) {
                    break;
                }
                test->step = STEP_SUBMIT_NEW_TERMINAL_RUN;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_SUBMIT_NEW_TERMINAL_RUN: {
                GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));
                GtkTreeIter toolbox_iter, script_iter;
                gtk_tree_model_get_iter_first(model, &toolbox_iter);
                if (!find_child_by_name(model, &toolbox_iter, "runner.sh", &script_iter)) {
                    fail(test, "'runner.sh' row not found under TOOLBOX");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *menu = open_menu_for_row(tree_view, model, &script_iter);
                if (!menu || !click_menu_item(menu, "Run with Arguments\xE2\x80\xA6")) {
                    fail(test, "could not click 'Run with Arguments...' on runner.sh");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *dialog = find_run_dialog();
                if (!dialog) {
                    fail(test, "Run with Arguments dialog did not appear");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *args_entry = find_by_data_key(dialog, "toolbox-run-arguments-entry");
                GtkWidget *env_entry = find_by_data_key(dialog, "toolbox-run-env-entry");
                if (!args_entry || !env_entry) {
                    fail(test, "Run with Arguments dialog fields not found");
                    return G_SOURCE_REMOVE;
                }
                char args_text[512];
                snprintf(args_text, sizeof(args_text), "%s --flag \"value with spaces\"", test->sentinel_new);
                gtk_entry_set_text(GTK_ENTRY(args_entry), args_text);
                gtk_entry_set_text(GTK_ENTRY(env_entry), "MARKER=hello world");
                /* "Create new terminal" defaults checked - leave it. */
                gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

                test->step = STEP_WAIT_NEW_TERMINAL_SENTINEL;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_NEW_TERMINAL_SENTINEL: {
                char content[512];
                if (!read_sentinel(test->sentinel_new, content, sizeof(content))) {
                    break;
                }
                char expected_pwd[4400];
                snprintf(expected_pwd, sizeof(expected_pwd), "PWD:%s", test->root.canonical_path);
                if (!strstr(content, expected_pwd)) {
                    fail(test, "runner.sh's pwd should match the script's own directory (the default working "
                               "directory)");
                }
                if (!strstr(content, "ARGS:--flag value with spaces")) {
                    fail(test, "runner.sh should have received --flag and the quoted multi-word argument");
                }
                if (!strstr(content, "MARKER:hello world")) {
                    fail(test, "runner.sh should have seen the MARKER environment override");
                }
                test->step = STEP_WAIT_NEW_TERMINAL_CAPTURE;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_NEW_TERMINAL_CAPTURE: {
                /* Run with Arguments (new terminal) spawns the process
                 * directly as argv - never "typed" through commit/
                 * terminal_send - so without terminal_run_command()'s own
                 * capture call, neither the command it ran nor that
                 * command's real terminal output (the unredirected
                 * STDOUT-MARKER echo, as opposed to the sentinel file
                 * above) would ever reach the database. */
                if (!event_was_captured("input", "runner.sh")) {
                    break; /* keep polling - the tick-driven capture may not have landed yet */
                }
                if (!event_was_captured("output", "STDOUT-MARKER:visible-in-terminal:--flag")) {
                    break;
                }
                test->step = STEP_VALIDATE_REJECTION;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_VALIDATE_REJECTION: {
                GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));
                GtkTreeIter toolbox_iter, script_iter;
                gtk_tree_model_get_iter_first(model, &toolbox_iter);
                find_child_by_name(model, &toolbox_iter, "runner.sh", &script_iter);
                GtkWidget *menu = open_menu_for_row(tree_view, model, &script_iter);
                if (!menu || !click_menu_item(menu, "Run with Arguments\xE2\x80\xA6")) {
                    fail(test, "could not click 'Run with Arguments...' on runner.sh (rejection check)");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *dialog = find_run_dialog();
                if (!dialog) {
                    fail(test, "Run with Arguments dialog did not appear (rejection check)");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *wd_entry = find_by_data_key(dialog, "toolbox-run-working-directory-entry");
                gtk_entry_set_text(GTK_ENTRY(wd_entry), "../escape");
                int terminals_before = count_terminal_pages(notebook);
                gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

                if (!GTK_IS_WIDGET(dialog) || !gtk_widget_get_visible(dialog)) {
                    fail(test, "an out-of-workspace working directory should leave the dialog open");
                    return G_SOURCE_REMOVE;
                }
                gboolean saw_error = FALSE;
                GPtrArray *labels = g_ptr_array_new();
                collect_by_type(dialog, labels, GTK_TYPE_LABEL);
                for (guint i = 0; i < labels->len; i++) {
                    const char *text = gtk_label_get_text(GTK_LABEL(g_ptr_array_index(labels, i)));
                    if (text && strstr(text, "isn't allowed")) {
                        saw_error = TRUE;
                        break;
                    }
                }
                g_ptr_array_free(labels, TRUE);
                if (!saw_error) {
                    fail(test, "an out-of-workspace working directory should show an inline error");
                }
                if (count_terminal_pages(notebook) != terminals_before) {
                    fail(test, "an out-of-workspace working directory must not spawn anything");
                }
                gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);

                test->step = STEP_SUBMIT_REUSE_RUN;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_SUBMIT_REUSE_RUN: {
                GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));
                GtkTreeIter toolbox_iter, script_iter;
                gtk_tree_model_get_iter_first(model, &toolbox_iter);
                find_child_by_name(model, &toolbox_iter, "runner.sh", &script_iter);
                test->terminals_before_reuse = count_terminal_pages(notebook);

                /* "Reuse" targets whichever terminal tab is currently
                 * active - the earlier new-terminal run already switched
                 * focus to *its* tab (add_tab_page's focus=TRUE), and
                 * that one's only child (runner.sh itself) has already
                 * exited by now. Switch back to the startup "Terminal 1"
                 * tab, which still has a live interactive shell, before
                 * testing reuse. */
                int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
                for (int i = 0; i < n; i++) {
                    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
                    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
                    if (tab && tab->type == TAB_TYPE_TERMINAL && strcmp(tab->title, "Terminal 1") == 0) {
                        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), i);
                        break;
                    }
                }

                GtkWidget *menu = open_menu_for_row(tree_view, model, &script_iter);
                if (!menu || !click_menu_item(menu, "Run with Arguments\xE2\x80\xA6")) {
                    fail(test, "could not click 'Run with Arguments...' on runner.sh (reuse check)");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *dialog = find_run_dialog();
                if (!dialog) {
                    fail(test, "Run with Arguments dialog did not appear (reuse check)");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *args_entry = find_by_data_key(dialog, "toolbox-run-arguments-entry");
                GtkWidget *env_entry = find_by_data_key(dialog, "toolbox-run-env-entry");
                GtkWidget *create_new_check = find_by_data_key(dialog, "toolbox-run-create-new-terminal-check");
                char args_text[512];
                snprintf(args_text, sizeof(args_text), "%s reused", test->sentinel_reuse);
                gtk_entry_set_text(GTK_ENTRY(args_entry), args_text);
                gtk_entry_set_text(GTK_ENTRY(env_entry), "MARKER=second run");
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(create_new_check), FALSE);
                gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

                test->step = STEP_WAIT_REUSE_SENTINEL;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_REUSE_SENTINEL: {
                if (count_terminal_pages(notebook) != test->terminals_before_reuse) {
                    fail(test, "reusing an existing terminal must not open a new terminal tab");
                    return G_SOURCE_REMOVE;
                }
                char content[512];
                if (!read_sentinel(test->sentinel_reuse, content, sizeof(content))) {
                    break;
                }
                if (!strstr(content, "ARGS:reused")) {
                    fail(test, "the reused terminal should have received the 'reused' argument");
                }
                if (!strstr(content, "MARKER:second run")) {
                    fail(test, "the reused terminal should have seen the MARKER environment override");
                }
                test->step = STEP_WAIT_REUSE_CAPTURE;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_REUSE_CAPTURE: {
                /* Reuse goes through terminal_send()/the commit-based
                 * capture path already fixed earlier - this just confirms
                 * it still works for a script run this way, and (via the
                 * per-invocation $* marker) that it's this run's own
                 * output being captured, not a leftover match from the
                 * new-terminal run above. */
                if (!event_was_captured("input", "reused")) {
                    break;
                }
                if (!event_was_captured("output", "STDOUT-MARKER:visible-in-terminal:reused")) {
                    break;
                }
                test->done = TRUE;
                gtk_widget_destroy(GTK_WIDGET(window));
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
        GtkWindow *window = main_window();
        if (window) {
            gtk_widget_destroy(GTK_WIDGET(window));
        }
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* Every GTK smoke test binary lives in the same build/tests/ directory,
 * so workspace_root_init()'s exe-relative resolution finds the *same*
 * physical files/ directory for all of them - clearing any pre-existing
 * top-level entries keeps this test's assertions correct regardless of
 * ctest run order, same precaution every explorer smoke test already
 * established. */
static void clear_workspace_root(const WorkspaceRoot *root) {
    DIR *dir = opendir(root->canonical_path);
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        file_delete(root, entry->d_name, true);
    }
    closedir(dir);
}

static void write_fixtures(const WorkspaceRoot *root) {
    char path[4400];
    snprintf(path, sizeof(path), "%s/runner.sh", root->canonical_path);
    const char *script =
        "#!/bin/sh\n"
        "SENTINEL=\"$1\"\n"
        "shift\n"
        "{\n"
        "  echo \"PWD:$(pwd)\"\n"
        "  echo \"ARGS:$*\"\n"
        "  echo \"MARKER:$MARKER\"\n"
        "} > \"$SENTINEL\"\n"
        /* Unlike the block above (redirected to the sentinel file, so the
         * test can assert on real spawned-process argv/cwd/env without
         * scraping terminal text), this one is deliberately NOT
         * redirected - it's real terminal output, there specifically to
         * verify Run in Terminal/Run with Arguments' output gets captured
         * into the database the same way typed-command output does. $*
         * (the received args) makes each invocation's echo distinct, so
         * the new-terminal and reuse-terminal runs can each be verified
         * independently rather than one check trivially matching the
         * other's already-captured output. */
        "echo \"STDOUT-MARKER:visible-in-terminal:$*\"\n";
    write_file_bytes(path, script, strlen(script));
    chmod(path, 0755);
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "terminal_run_command_smoke: app_create failed\n");
        return 1;
    }
    test.root = *app_get_file_workspace_root(app);
    clear_workspace_root(&test.root);
    write_fixtures(&test.root);

    /* toolbox.db is shared across every App-backed test in this suite -
     * start from a clean slate so the capture assertions below can't pass
     * by matching a leftover row from an earlier test run. */
    database_clear_terminal_events();

    snprintf(test.sentinel_new, sizeof(test.sentinel_new), "/tmp/toolbox_run_cmd_smoke_%d_new", (int)getpid());
    snprintf(test.sentinel_reuse, sizeof(test.sentinel_reuse), "/tmp/toolbox_run_cmd_smoke_%d_reuse", (int)getpid());
    unlink(test.sentinel_new);
    unlink(test.sentinel_reuse);

    g_timeout_add(STEP_INTERVAL_MS, drive, &test);

    int status = app_run(app);
    app_destroy(app);

    unlink(test.sentinel_new);
    unlink(test.sentinel_reuse);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "terminal_run_command_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "terminal_run_command_smoke: test did not complete\n");
        return 1;
    }

    g_print("terminal_run_command_smoke: Run with Arguments (new terminal - argv/cwd/env all reached the real "
            "process, and both the run command and its output were captured), outside-workspace working-directory "
            "rejection, and reuse-existing-terminal (capture verified too) all verified\n");
    return 0;
}
