/*
 * Exercises FileWatcher wired into the real running app: a file created,
 * deleted, and renamed directly by this test process (simulating an
 * external terminal or editor touching files/ from outside the app -
 * never through file_operations.c or the explorer UI) is reflected in
 * the TOOLBOX tree with no manual Refresh click, driven entirely by
 * on_tick's own 100ms drain of backend->file_watcher. Watching is
 * FILES-only this step (see ui_gtk_file_tree.c's
 * register_watch_for_loaded_row comment), so this test only exercises
 * the TOOLBOX root, which is already expanded/watched from startup.
 */
#include <dirent.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/app.h"
#include "files/file_operations.h"
#include "files/workspace_root.h"

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
#define STEP_TIMEOUT_MS 5000

typedef enum Stage {
    STAGE_CREATE_TRIGGER,
    STAGE_WAIT_CREATED,
    STAGE_DELETE_TRIGGER,
    STAGE_WAIT_DELETED,
    STAGE_RENAME_SETUP,
    STAGE_WAIT_RENAME_SOURCE,
    STAGE_RENAME_TRIGGER,
    STAGE_WAIT_RENAMED,
} Stage;

typedef struct TestState {
    Stage stage;
    int elapsed_ms;
    gboolean failed;
    gboolean done;
    WorkspaceRoot root;
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
    fprintf(stderr, "file_watch_explorer_smoke: %s\n", msg);
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

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static gboolean drive(gpointer user_data) {
    TestState *test = user_data;

    GtkWindow *window = main_window();
    GtkWidget *tree_view = window ? find_by_data_key(GTK_WIDGET(window), "toolbox-explorer-tree") : NULL;
    if (!tree_view) {
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
    GtkTreeIter toolbox_iter;
    if (!gtk_tree_model_get_iter_first(model, &toolbox_iter) || !row_name_is(model, &toolbox_iter, "TOOLBOX")) {
        fail(test, "expected TOOLBOX as the first top-level row");
        goto done;
    }

    char created_path[4200];
    snprintf(created_path, sizeof(created_path), "%s/external.txt", test->root.canonical_path);
    char rename_from_path[4200], rename_to_path[4200];
    snprintf(rename_from_path, sizeof(rename_from_path), "%s/before_rename.txt", test->root.canonical_path);
    snprintf(rename_to_path, sizeof(rename_to_path), "%s/after_rename.txt", test->root.canonical_path);

    test->elapsed_ms += STEP_INTERVAL_MS;

    switch (test->stage) {
        case STAGE_CREATE_TRIGGER:
            write_file(created_path, "hello from outside");
            test->stage = STAGE_WAIT_CREATED;
            test->elapsed_ms = 0;
            return G_SOURCE_CONTINUE;

        case STAGE_WAIT_CREATED: {
            GtkTreeIter found;
            if (find_child_by_name(model, &toolbox_iter, "external.txt", &found)) {
                test->stage = STAGE_DELETE_TRIGGER;
                test->elapsed_ms = 0;
            } else if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
                fail(test, "externally created 'external.txt' never appeared under TOOLBOX without a manual refresh");
                goto done;
            }
            return G_SOURCE_CONTINUE;
        }

        case STAGE_DELETE_TRIGGER:
            unlink(created_path);
            test->stage = STAGE_WAIT_DELETED;
            test->elapsed_ms = 0;
            return G_SOURCE_CONTINUE;

        case STAGE_WAIT_DELETED: {
            GtkTreeIter found;
            if (!find_child_by_name(model, &toolbox_iter, "external.txt", &found)) {
                test->stage = STAGE_RENAME_SETUP;
                test->elapsed_ms = 0;
            } else if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
                fail(test, "externally deleted 'external.txt' row should have disappeared without a manual refresh");
                goto done;
            }
            return G_SOURCE_CONTINUE;
        }

        case STAGE_RENAME_SETUP:
            write_file(rename_from_path, "will be renamed");
            test->stage = STAGE_WAIT_RENAME_SOURCE;
            test->elapsed_ms = 0;
            return G_SOURCE_CONTINUE;

        case STAGE_WAIT_RENAME_SOURCE: {
            GtkTreeIter found;
            if (find_child_by_name(model, &toolbox_iter, "before_rename.txt", &found)) {
                test->stage = STAGE_RENAME_TRIGGER;
                test->elapsed_ms = 0;
            } else if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
                fail(test, "'before_rename.txt' never appeared before the rename trigger");
                goto done;
            }
            return G_SOURCE_CONTINUE;
        }

        case STAGE_RENAME_TRIGGER:
            rename(rename_from_path, rename_to_path);
            test->stage = STAGE_WAIT_RENAMED;
            test->elapsed_ms = 0;
            return G_SOURCE_CONTINUE;

        case STAGE_WAIT_RENAMED: {
            GtkTreeIter old_row, new_row;
            gboolean old_gone = !find_child_by_name(model, &toolbox_iter, "before_rename.txt", &old_row);
            gboolean new_here = find_child_by_name(model, &toolbox_iter, "after_rename.txt", &new_row);
            if (old_gone && new_here) {
                test->done = TRUE;
                goto done;
            }
            if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
                fail(test, "externally renamed row should update to 'after_rename.txt' without a manual refresh");
                goto done;
            }
            return G_SOURCE_CONTINUE;
        }
    }

done:
    gtk_window_close(window);
    return G_SOURCE_REMOVE;
}

/* Every GTK smoke test binary lives in the same build/tests/ directory,
 * so workspace_root_init()'s exe-relative resolution finds the *same*
 * physical files/ directory for all of them - see
 * explorer_operations_smoke.c's own established fix for this. */
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

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "file_watch_explorer_smoke: app_create failed\n");
        return 1;
    }
    test.root = *app_get_file_workspace_root(app);
    clear_workspace_root(&test.root);

    g_timeout_add(STEP_INTERVAL_MS, drive, &test);

    int status = app_run(app);
    app_destroy(app);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "file_watch_explorer_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "file_watch_explorer_smoke: test did not complete\n");
        return 1;
    }

    g_print("file_watch_explorer_smoke: external create/delete/rename all reflected in the explorer tree with no "
            "manual refresh\n");
    return 0;
}
