/*
 * Exercises Step 8's checkpoint end-to-end in the real app: the top bar
 * Search button opens the search window; a query (Enter, and separately
 * the Search button) returns file/folder name matches from both TOOLBOX
 * and Toolkit with the correct "<root>/relative_path" display text and
 * status-label wording (including the zero-match case); activating a
 * file result opens it as an editor tab; activating a folder result
 * reveals/selects it in the main explorer tree, loading not-yet-
 * expanded ancestor directories on demand; Match Case actually changes
 * what matches; and clicking the top bar Search button again presents
 * the existing window rather than opening a second one.
 */
#include <dirent.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/app.h"
#include "core/tab.h"
#include "files/file_operations.h"
#include "files/workspace_root.h"
#include "test_gtk_utils.h"

#define STEP_INTERVAL_MS 100
#define STEP_TIMEOUT_MS 5000

typedef struct TestState {
    int elapsed_ms;
    gboolean failed;
    gboolean done;
    gboolean close_triggered; /* gtk_window_close() queues delete-event rather than delivering it
                                * synchronously - once TRUE, drive() only polls for the search
                                * window's disappearance rather than touching the (possibly by
                                * then destroyed) main window's own widgets. */
    WorkspaceRoot files_root;
    WorkspaceRoot toolkit_root;
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
    /* gtk_application_get_windows() returns most-recently-added first -
     * the main window is the last one added (added at startup, before
     * anything else), so it's the last element, not necessarily the
     * first once the search window is also registered. */
    GtkWindow *result = NULL;
    for (GList *l = windows; l; l = l->next) {
        result = GTK_WINDOW(l->data);
    }
    return result;
}

static GtkWindow *find_window_by_title(const char *title) {
    GList *toplevels = gtk_window_list_toplevels();
    GtkWindow *result = NULL;
    for (GList *l = toplevels; l; l = l->next) {
        if (GTK_IS_WINDOW(l->data)) {
            const char *window_title = gtk_window_get_title(GTK_WINDOW(l->data));
            if (window_title && strcmp(window_title, title) == 0) {
                result = GTK_WINDOW(l->data);
                break;
            }
        }
    }
    g_list_free(toplevels);
    return result;
}

static int count_windows_by_title(const char *title) {
    GList *toplevels = gtk_window_list_toplevels();
    int count = 0;
    for (GList *l = toplevels; l; l = l->next) {
        if (GTK_IS_WINDOW(l->data)) {
            const char *window_title = gtk_window_get_title(GTK_WINDOW(l->data));
            if (window_title && strcmp(window_title, title) == 0) {
                count++;
            }
        }
    }
    g_list_free(toplevels);
    return count;
}

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "explorer_search_smoke: %s\n", msg);
    test->failed = TRUE;
}

static GtkWidget *find_editor_page(GtkWidget *notebook, const char *title) {
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
        if (tab && tab->type == TAB_TYPE_EDITOR && strcmp(tab->title, title) == 0) {
            return page;
        }
    }
    return NULL;
}

static gboolean results_contain(GtkTreeModel *model, const char *display_text) {
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_first(model, &iter)) {
        return FALSE;
    }
    do {
        gchar *text = NULL;
        gtk_tree_model_get(model, &iter, 0 /* SEARCH_COL_DISPLAY_PATH */, &text, -1);
        gboolean match = text && strcmp(text, display_text) == 0;
        g_free(text);
        if (match) {
            return TRUE;
        }
    } while (gtk_tree_model_iter_next(model, &iter));
    return FALSE;
}

/* Activates the results-tree row whose display text matches exactly -
 * search result order across two roots isn't guaranteed stable within
 * a single root's own matches (readdir() order), so tests locate the
 * row they want by content rather than assuming a fixed index. */
static gboolean activate_result(GtkWidget *results_tree, GtkTreeModel *model, const char *display_text) {
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_first(model, &iter)) {
        return FALSE;
    }
    do {
        gchar *text = NULL;
        gtk_tree_model_get(model, &iter, 0 /* SEARCH_COL_DISPLAY_PATH */, &text, -1);
        gboolean match = text && strcmp(text, display_text) == 0;
        g_free(text);
        if (match) {
            GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
            gtk_tree_view_row_activated(GTK_TREE_VIEW(results_tree), path,
                                         gtk_tree_view_get_column(GTK_TREE_VIEW(results_tree), 0));
            gtk_tree_path_free(path);
            return TRUE;
        }
    } while (gtk_tree_model_iter_next(model, &iter));
    return FALSE;
}

enum {
    EXPLORER_COL_ICON,
    EXPLORER_COL_NAME,
    EXPLORER_COL_PATH,
    EXPLORER_COL_IS_DIR,
    EXPLORER_COL_LOADED,
    EXPLORER_COL_SOURCE,
    EXPLORER_COL_NODE_ID
};

static gboolean explorer_row_name_is(GtkTreeModel *model, GtkTreeIter *iter, const char *expected) {
    gchar *name = NULL;
    gtk_tree_model_get(model, iter, EXPLORER_COL_NAME, &name, -1);
    gboolean match = name && strcmp(name, expected) == 0;
    g_free(name);
    return match;
}

static gboolean explorer_find_child(GtkTreeModel *model, GtkTreeIter *parent, const char *name, GtkTreeIter *out) {
    GtkTreeIter child;
    if (!gtk_tree_model_iter_children(model, &child, parent)) {
        return FALSE;
    }
    do {
        if (explorer_row_name_is(model, &child, name)) {
            *out = child;
            return TRUE;
        }
    } while (gtk_tree_model_iter_next(model, &child));
    return FALSE;
}

/* gtk_window_close() queues delete-event rather than delivering it
 * synchronously, so the actual close-confirmation-then-destroy chain
 * (and this app's own search-window-follows-main-window cleanup)
 * completes on a later main loop iteration, not within the call that
 * triggered it - and can complete late enough that no further
 * g_timeout_add tick ever runs before the whole app quits (once every
 * window is gone, g_application_run() returns). Rather than poll for
 * that from drive() itself (racing against the app's own shutdown),
 * this connects directly to the search window's "destroy" signal so
 * test->done is set synchronously the moment it actually happens,
 * whenever that is relative to app_run() returning. */
static void on_test_search_window_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    TestState *test = user_data;
    test->done = TRUE;
}

static gboolean drive(gpointer user_data) {
    TestState *test = user_data;

    if (test->close_triggered) {
        /* test->done is set by on_test_search_window_destroy() above,
         * not by this polling loop - this is just a bounded fail-safe
         * in case the search window never closes at all (a genuine
         * regression), which would otherwise hang until ctest's own
         * per-test timeout. */
        if (test->done) {
            return G_SOURCE_REMOVE;
        }
        test->elapsed_ms += STEP_INTERVAL_MS;
        if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
            fail(test, "closing the main window should also close the search window");
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    GtkWindow *window = main_window();
    GtkWidget *search_open_button = window ? find_by_data_key(GTK_WIDGET(window), "workbench-search-open-button")
                                            : NULL;
    if (!search_open_button) {
        test->elapsed_ms += STEP_INTERVAL_MS;
        if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
            fail(test, "the top bar's Search button never appeared");
            if (window) {
                gtk_window_close(window);
            }
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    /* Open the search window. */
    gtk_button_clicked(GTK_BUTTON(search_open_button));
    GtkWindow *search_window = find_window_by_title("Search");
    if (!search_window) {
        fail(test, "clicking the Search button should open a window titled 'Search'");
        goto done;
    }
    g_signal_connect(search_window, "destroy", G_CALLBACK(on_test_search_window_destroy), test);

    GtkWidget *entry = find_by_data_key(GTK_WIDGET(search_window), "workbench-search-entry");
    GtkWidget *match_case = find_by_data_key(GTK_WIDGET(search_window), "workbench-search-match-case");
    GtkWidget *search_button = find_by_data_key(GTK_WIDGET(search_window), "workbench-search-button");
    GtkWidget *results_tree = find_by_data_key(GTK_WIDGET(search_window), "workbench-search-results-tree");
    GtkWidget *status_label = find_by_data_key(GTK_WIDGET(search_window), "workbench-search-status-label");
    if (!entry || !match_case || !search_button || !results_tree || !status_label) {
        fail(test, "the search window is missing one of its expected widgets");
        goto done;
    }

    /* Enter-triggered, case-insensitive by default: a file directly
     * under TOOLBOX, a nested folder under TOOLBOX (inside an
     * unexpanded "container" folder), and a file under Toolkit all
     * match by name. */
    gtk_entry_set_text(GTK_ENTRY(entry), "needle");
    g_signal_emit_by_name(entry, "activate");

    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(results_tree));
    if (gtk_tree_model_iter_n_children(model, NULL) != 3) {
        fail(test, "searching 'needle' should return exactly 3 matches");
        goto done;
    }
    if (!results_contain(model, "TOOLBOX/needlefile.txt") ||
        !results_contain(model, "TOOLBOX/container/needlefolder") ||
        !results_contain(model, "Toolkit/needlebeta.txt")) {
        fail(test, "expected matches for TOOLBOX/needlefile.txt, TOOLBOX/container/needlefolder, and "
                   "Toolkit/needlebeta.txt");
        goto done;
    }
    if (strcmp(gtk_label_get_text(GTK_LABEL(status_label)),
               "3 items found matching \xE2\x80\x9Cneedle\xE2\x80\x9D.") != 0) {
        fail(test, "the status label should read '3 items found matching \xE2\x80\x9Cneedle\xE2\x80\x9D.'");
        goto done;
    }

    /* Activating the file result opens it as an editor tab. */
    GPtrArray *notebooks = g_ptr_array_new();
    collect_by_type(GTK_WIDGET(window), notebooks, GTK_TYPE_NOTEBOOK);
    GtkWidget *notebook = notebooks->len > 0 ? GTK_WIDGET(g_ptr_array_index(notebooks, 0)) : NULL;
    g_ptr_array_free(notebooks, TRUE);
    if (!notebook) {
        fail(test, "main window notebook not found");
        goto done;
    }

    if (!activate_result(results_tree, model, "TOOLBOX/needlefile.txt")) {
        fail(test, "the 'TOOLBOX/needlefile.txt' result row was not found to activate");
        goto done;
    }
    if (!find_editor_page(notebook, "needlefile.txt")) {
        fail(test, "activating the file result should open 'needlefile.txt' as an editor tab");
        goto done;
    }

    /* Activating the folder result reveals/selects it in the main
     * explorer tree, loading "container" (not yet expanded/loaded) on
     * demand - not just working because the row already happened to be
     * visible. */
    GtkWidget *explorer_tree = find_by_data_key(GTK_WIDGET(window), "workbench-explorer-tree");
    if (!explorer_tree) {
        fail(test, "explorer tree view not found");
        goto done;
    }
    GtkTreeModel *explorer_model = gtk_tree_view_get_model(GTK_TREE_VIEW(explorer_tree));
    GtkTreeIter workbench_iter, container_iter;
    if (!gtk_tree_model_get_iter_first(explorer_model, &workbench_iter) ||
        !explorer_row_name_is(explorer_model, &workbench_iter, "TOOLBOX")) {
        fail(test, "expected TOOLBOX as the first top-level row");
        goto done;
    }
    if (!explorer_find_child(explorer_model, &workbench_iter, "container", &container_iter)) {
        fail(test, "'container' row not found under TOOLBOX");
        goto done;
    }
    gboolean container_loaded_before = FALSE;
    gtk_tree_model_get(explorer_model, &container_iter, EXPLORER_COL_LOADED, &container_loaded_before, -1);
    if (container_loaded_before) {
        fail(test, "'container' should not be loaded/expanded yet - this scenario needs an unexpanded ancestor");
        goto done;
    }

    if (!activate_result(results_tree, model, "TOOLBOX/container/needlefolder")) {
        fail(test, "the 'TOOLBOX/container/needlefolder' result row was not found to activate");
        goto done;
    }

    /* Re-fetch container (reveal's own load_row_children reload may
     * have replaced the iter) and confirm needlefolder is now loaded,
     * present, and selected. */
    if (!explorer_find_child(explorer_model, &workbench_iter, "container", &container_iter)) {
        fail(test, "'container' row missing after reveal");
        goto done;
    }
    GtkTreeIter needlefolder_iter;
    if (!explorer_find_child(explorer_model, &container_iter, "needlefolder", &needlefolder_iter)) {
        fail(test, "revealing 'container/needlefolder' should load 'container's children");
        goto done;
    }
    if (!gtk_tree_selection_iter_is_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(explorer_tree)),
                                              &needlefolder_iter)) {
        fail(test, "revealing a folder result should select it in the explorer tree");
        goto done;
    }

    /* Match Case actually changes what matches - "NEEDLE" (uppercase)
     * against lowercase fixture names: 0 matches with Match Case on, 3
     * with it off again. */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(match_case), TRUE);
    gtk_entry_set_text(GTK_ENTRY(entry), "NEEDLE");
    gtk_button_clicked(GTK_BUTTON(search_button));
    if (gtk_tree_model_iter_n_children(model, NULL) != 0) {
        fail(test, "case-sensitive 'NEEDLE' should match nothing against lowercase fixture names");
        goto done;
    }
    /* A genuine zero-match search must say so explicitly - an empty
     * results list alone looks identical to "the search never ran." */
    if (strcmp(gtk_label_get_text(GTK_LABEL(status_label)),
               "No items found matching \xE2\x80\x9CNEEDLE\xE2\x80\x9D.") != 0) {
        fail(test, "a zero-match search should show 'No items found matching \xE2\x80\x9CNEEDLE\xE2\x80\x9D.'");
        goto done;
    }

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(match_case), FALSE);
    gtk_button_clicked(GTK_BUTTON(search_button));
    if (gtk_tree_model_iter_n_children(model, NULL) != 3) {
        fail(test, "turning Match Case back off should restore the 3 case-insensitive matches");
        goto done;
    }

    /* Clicking the top bar Search button again presents the same
     * window rather than opening a duplicate. */
    gtk_button_clicked(GTK_BUTTON(search_open_button));
    if (count_windows_by_title("Search") != 1) {
        fail(test, "clicking Search again should not open a second search window");
        goto done;
    }

    /* Closing the main window must also close the search window - it
     * must never become an orphan keeping the process alive. Triggers
     * the close and switches into the close_triggered polling branch at
     * the top of this function on the next tick - gtk_window_close()
     * queues delete-event rather than delivering it synchronously. */
    gtk_window_close(window);
    test->close_triggered = TRUE;
    test->elapsed_ms = 0;
    return G_SOURCE_CONTINUE;

done:
    gtk_window_close(window);
    return G_SOURCE_REMOVE;
}

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

static void write_file_bytes(const char *path, const char *content, size_t len) {
    FILE *f = fopen(path, "wb");
    if (f) {
        if (len > 0) {
            fwrite(content, 1, len, f);
        }
        fclose(f);
    }
}

static void write_fixtures(const WorkspaceRoot *files_root, const WorkspaceRoot *toolkit_root) {
    char path[4400];

    snprintf(path, sizeof(path), "%s/needlefile.txt", files_root->canonical_path);
    write_file_bytes(path, "irrelevant content\n", strlen("irrelevant content\n"));

    /* "needlefolder" nested inside "container" (itself not matching
     * "needle") - proves reveal_in_explorer() loads a not-yet-expanded
     * ancestor on demand, not just working on an already-visible row. */
    snprintf(path, sizeof(path), "%s/container", files_root->canonical_path);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/container/needlefolder", files_root->canonical_path);
    mkdir(path, 0755);

    snprintf(path, sizeof(path), "%s/needlebeta.txt", toolkit_root->canonical_path);
    write_file_bytes(path, "irrelevant content\n", strlen("irrelevant content\n"));
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "explorer_search_smoke: app_create failed\n");
        return 1;
    }
    test.files_root = *app_get_file_workspace_root(app);
    test.toolkit_root = *app_get_toolkit_workspace_root(app);
    clear_workspace_root(&test.files_root);
    clear_workspace_root(&test.toolkit_root);
    write_fixtures(&test.files_root, &test.toolkit_root);

    g_timeout_add(STEP_INTERVAL_MS, drive, &test);

    int status = app_run(app);
    app_destroy(app);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "explorer_search_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "explorer_search_smoke: test did not complete\n");
        return 1;
    }

    g_print("explorer_search_smoke: opening the search window, cross-root file/folder name matches with correct "
            "display text and status wording, file-result open and folder-result reveal-in-explorer (including "
            "on-demand ancestor loading), Match Case, singleton-window behavior, and closing with the main window "
            "all verified\n");
    return 0;
}
