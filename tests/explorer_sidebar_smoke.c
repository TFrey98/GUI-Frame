/*
 * Exercises Step 1's checkpoint end-to-end in the real app: the merged
 * explorer sidebar shows two permanent top-level rows (TOOLBOX, backed
 * by FileTree/files/; Toolkit, backed by toolkit_index/toolkit/), each
 * loads its own immediate contents eagerly with no click needed,
 * subfolders lazy-load only once expanded, Refresh doesn't duplicate
 * rows, and the two sources never cross-contaminate.
 */
#include <dirent.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/app.h"
#include "files/workspace_root.h"
#include "test_gtk_utils.h"
#include "tools/toolkit_index.h"

#define STEP_INTERVAL_MS 100
#define STEP_TIMEOUT_MS 3000

/* Mirrors ui_gtk.c's file-local EXPLORER_COL_* enum - not a public API,
 * same convention other GTK smoke tests already use for ui_gtk.c-
 * internal tree/column data. */
enum {
    EXPLORER_COL_ICON,
    EXPLORER_COL_NAME,
    EXPLORER_COL_PATH,
    EXPLORER_COL_IS_DIR,
    EXPLORER_COL_LOADED,
    EXPLORER_COL_SOURCE,
    EXPLORER_COL_NODE_ID
};

typedef struct TestState {
    int elapsed_ms;
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

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "explorer_sidebar_smoke: %s\n", msg);
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

static void write_fixture_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (f) {
        fclose(f);
    }
}

/* Every GTK smoke test binary lives in the same build/tests/ directory,
 * so workspace_root_init()'s (and toolkit_index's) exe-relative
 * resolution finds the *same* physical files/toolkit directories for
 * all of them - ctest runs them back to back with no cleanup in
 * between. Clearing any pre-existing entries here keeps this test's
 * exact-child-count assertions correct regardless of run order. */
static void clear_directory_absolute(const char *absolute_path) {
    DIR *dir = opendir(absolute_path);
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[4400];
        snprintf(child, sizeof(child), "%s/%s", absolute_path, entry->d_name);
        struct stat st;
        if (lstat(child, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            clear_directory_absolute(child);
            rmdir(child);
        } else {
            unlink(child);
        }
    }
    closedir(dir);
}

static gboolean drive(gpointer user_data) {
    TestState *test = user_data;

    GtkWindow *window = main_window();
    GtkWidget *tree_view = window ? find_by_data_key(GTK_WIDGET(window), "workbench-explorer-tree") : NULL;
    if (!tree_view) {
        /* The window may not have finished its initial build on the
         * very first tick - wait rather than fail immediately. */
        test->elapsed_ms += STEP_INTERVAL_MS;
        if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
            fail(test, "explorer tree view never appeared");
            if (window) {
                gtk_window_close(window);
            }
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));

    GtkTreeIter workbench_iter;
    if (!gtk_tree_model_get_iter_first(model, &workbench_iter)) {
        fail(test, "no top-level rows");
        goto done;
    }
    if (!row_name_is(model, &workbench_iter, "TOOLBOX")) {
        fail(test, "expected first top-level row to be TOOLBOX");
        goto done;
    }

    GtkTreeIter toolkit_iter = workbench_iter;
    if (!gtk_tree_model_iter_next(model, &toolkit_iter) || !row_name_is(model, &toolkit_iter, "Toolkit")) {
        fail(test, "expected second top-level row to be Toolkit");
        goto done;
    }

    GtkTreeIter extra_root = toolkit_iter;
    if (gtk_tree_model_iter_next(model, &extra_root)) {
        fail(test, "expected exactly two top-level rows");
        goto done;
    }

    /* TOOLBOX's own contents should already be populated with no click
     * needed - "load only the root initially." */
    if (gtk_tree_model_iter_n_children(model, &workbench_iter) != 2) {
        fail(test, "expected TOOLBOX to show its 2 fixture entries already");
        goto done;
    }

    GtkTreeIter afolder_iter;
    if (!find_child_by_name(model, &workbench_iter, "afolder", &afolder_iter)) {
        fail(test, "expected 'afolder' under TOOLBOX");
        goto done;
    }
    GtkTreeIter bfile_iter;
    if (!find_child_by_name(model, &workbench_iter, "bfile.txt", &bfile_iter)) {
        fail(test, "expected 'bfile.txt' under TOOLBOX");
        goto done;
    }

    /* Expanding afolder lazily loads its own nested fixture. */
    GtkTreePath *afolder_path = gtk_tree_model_get_path(model, &afolder_iter);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_view), afolder_path, FALSE);
    gtk_tree_path_free(afolder_path);
    if (gtk_tree_model_iter_n_children(model, &afolder_iter) != 1) {
        fail(test, "expected 1 child under afolder after expanding");
        goto done;
    }
    GtkTreeIter nested_iter;
    if (!gtk_tree_model_iter_children(model, &nested_iter, &afolder_iter) ||
        !row_name_is(model, &nested_iter, "nested.txt")) {
        fail(test, "expected 'nested.txt' inside afolder");
        goto done;
    }

    /* Refresh must not duplicate TOOLBOX's top-level rows. */
    GtkWidget *refresh_button = find_by_data_key(GTK_WIDGET(window), "workbench-explorer-refresh-button");
    if (!refresh_button) {
        fail(test, "refresh button not found");
        goto done;
    }
    gtk_button_clicked(GTK_BUTTON(refresh_button));
    if (gtk_tree_model_iter_n_children(model, &workbench_iter) != 2) {
        fail(test, "expected TOOLBOX to still show exactly 2 entries after refresh (no duplicates)");
        goto done;
    }

    /* The two sources never cross-contaminate. */
    if (gtk_tree_model_iter_n_children(model, &toolkit_iter) != 1) {
        fail(test, "expected exactly 1 entry under Toolkit");
        goto done;
    }
    GtkTreeIter tool_iter;
    if (!find_child_by_name(model, &toolkit_iter, "tool_a.sh", &tool_iter)) {
        fail(test, "expected 'tool_a.sh' under Toolkit");
        goto done;
    }
    if (find_child_by_name(model, &workbench_iter, "tool_a.sh", &tool_iter)) {
        fail(test, "Toolkit's fixture leaked into TOOLBOX");
        goto done;
    }
    if (find_child_by_name(model, &toolkit_iter, "afolder", &afolder_iter) ||
        find_child_by_name(model, &toolkit_iter, "bfile.txt", &bfile_iter)) {
        fail(test, "TOOLBOX's fixtures leaked into Toolkit");
        goto done;
    }

    test->done = TRUE;

done:
    gtk_window_close(window);
    return G_SOURCE_REMOVE;
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "explorer_sidebar_smoke: app_create failed\n");
        return 1;
    }

    /* Written directly to disk before app_run() ever builds the window -
     * the initial root-level scan (inside build_explorer_sidebar, only
     * reached once the GTK main loop starts) picks them up naturally,
     * no test-only hook needed. */
    const WorkspaceRoot *ws = app_get_file_workspace_root(app);
    clear_directory_absolute(ws->canonical_path);
    clear_directory_absolute(toolkit_index_dir());

    char afolder[4200], nested[4300], bfile[4200], tool_file[4200];
    snprintf(afolder, sizeof(afolder), "%s/afolder", ws->canonical_path);
    snprintf(nested, sizeof(nested), "%s/nested.txt", afolder);
    snprintf(bfile, sizeof(bfile), "%s/bfile.txt", ws->canonical_path);
    snprintf(tool_file, sizeof(tool_file), "%s/tool_a.sh", toolkit_index_dir());

    mkdir(afolder, 0755);
    write_fixture_file(nested);
    write_fixture_file(bfile);
    write_fixture_file(tool_file);

    g_timeout_add(STEP_INTERVAL_MS, drive, &test);

    int status = app_run(app);
    app_destroy(app);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "explorer_sidebar_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "explorer_sidebar_smoke: test did not complete\n");
        return 1;
    }

    g_print("explorer_sidebar_smoke: merged TOOLBOX/Toolkit roots, eager root load, lazy subfolder expand, "
            "refresh-without-duplicating, and source isolation all verified\n");
    return 0;
}
