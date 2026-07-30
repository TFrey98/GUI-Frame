/*
 * Exercises Step 3's checkpoint end-to-end in the real app: double-
 * clicking (row-activated) a text file opens a real TAB_TYPE_EDITOR
 * tab with the right title/content; activating the same row again
 * does not open a duplicate tab; a file containing a NUL byte opens a
 * TAB_TYPE_BINARY_INFO tab with correct metadata instead of a garbled
 * editor; a directory row still just expands/collapses, no tab opens;
 * a chmod-read-only text file opens an editor tab showing the
 * read-only indicator with its GtkTextView non-editable.
 *
 * Drives the real "row-activated" signal directly (gtk_tree_view_row_activated),
 * the same "real GTK signal, not simulated input" convention this
 * suite already established for "popup-menu" and cell-renderer
 * "edited" in explorer_operations_smoke.c.
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

typedef struct TestState {
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
    fprintf(stderr, "explorer_editor_smoke: %s\n", msg);
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

/* The real double-click handler - a direct call into the real GTK API
 * (not a synthesized click), same "real signal" philosophy already
 * established by this suite's other explorer smoke tests. */
static void activate_row(GtkWidget *tree_view, GtkTreeModel *model, GtkTreeIter *iter) {
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    GtkTreeViewColumn *column = gtk_tree_view_get_column(GTK_TREE_VIEW(tree_view), 0);
    gtk_tree_view_row_activated(GTK_TREE_VIEW(tree_view), path, column);
    gtk_tree_path_free(path);
}

static int tabs_of_type(GtkWidget *notebook, TabType type, GtkWidget **out, int max_out) {
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    int count = 0;
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
        if (tab && tab->type == type) {
            if (out && count < max_out) {
                out[count] = page;
            }
            count++;
        }
    }
    return count;
}

static char *text_view_contents(GtkWidget *page) {
    GtkWidget *view = g_object_get_data(G_OBJECT(page), "toolbox-editor-text-view");
    if (!view) {
        return NULL;
    }
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static GtkWidget *grid_value_at(GtkWidget *page, int row) {
    GPtrArray *grids = g_ptr_array_new();
    collect_by_type(page, grids, GTK_TYPE_GRID);
    if (grids->len == 0) {
        g_ptr_array_free(grids, TRUE);
        return NULL;
    }
    GtkGrid *grid = GTK_GRID(g_ptr_array_index(grids, 0));
    g_ptr_array_free(grids, TRUE);
    return gtk_grid_get_child_at(grid, 1, row);
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

static gboolean drive(gpointer user_data) {
    TestState *test = user_data;

    GtkWindow *window = main_window();
    GtkWidget *tree_view = window ? find_by_data_key(GTK_WIDGET(window), "toolbox-explorer-tree") : NULL;
    GPtrArray *notebooks = g_ptr_array_new();
    if (window) {
        collect_by_type(GTK_WIDGET(window), notebooks, GTK_TYPE_NOTEBOOK);
    }
    GtkWidget *notebook = notebooks->len > 0 ? GTK_WIDGET(g_ptr_array_index(notebooks, 0)) : NULL;
    g_ptr_array_free(notebooks, TRUE);

    if (!tree_view || !notebook) {
        test->elapsed_ms += STEP_INTERVAL_MS;
        if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
            fail(test, "explorer tree view or notebook never appeared");
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

    /* Activating a text file row opens a real editor tab with the
     * right title and content. */
    GtkTreeIter text_iter;
    if (!find_child_by_name(model, &toolbox_iter, "notes.txt", &text_iter)) {
        fail(test, "'notes.txt' row not found under TOOLBOX");
        goto done;
    }
    activate_row(tree_view, model, &text_iter);

    GtkWidget *editor_pages[4];
    int editor_count = tabs_of_type(notebook, TAB_TYPE_EDITOR, editor_pages, 4);
    if (editor_count != 1) {
        fail(test, "activating notes.txt should open exactly one TAB_TYPE_EDITOR tab");
        goto done;
    }
    Tab *notes_tab = g_object_get_data(G_OBJECT(editor_pages[0]), "toolbox-tab");
    if (!notes_tab || strcmp(notes_tab->title, "notes.txt") != 0) {
        fail(test, "notes.txt's editor tab should be titled 'notes.txt'");
        goto done;
    }
    char *contents = text_view_contents(editor_pages[0]);
    if (!contents || strcmp(contents, "hello from notes\n") != 0) {
        fail(test, "notes.txt's editor tab should show the file's real content");
        test->failed = TRUE;
    }
    g_free(contents);

    /* Activating the same row again does not open a duplicate tab. */
    activate_row(tree_view, model, &text_iter);
    if (tabs_of_type(notebook, TAB_TYPE_EDITOR, NULL, 0) != 1) {
        fail(test, "re-activating notes.txt should not open a duplicate editor tab");
        goto done;
    }

    /* A file with an embedded NUL byte opens a binary-info tab, not a
     * garbled editor, with correct metadata. */
    GtkTreeIter binary_iter;
    if (!find_child_by_name(model, &toolbox_iter, "data.bin", &binary_iter)) {
        fail(test, "'data.bin' row not found under TOOLBOX");
        goto done;
    }
    activate_row(tree_view, model, &binary_iter);

    GtkWidget *binary_pages[4];
    int binary_count = tabs_of_type(notebook, TAB_TYPE_BINARY_INFO, binary_pages, 4);
    if (binary_count != 1) {
        fail(test, "activating data.bin should open exactly one TAB_TYPE_BINARY_INFO tab");
        goto done;
    }
    if (tabs_of_type(notebook, TAB_TYPE_EDITOR, NULL, 0) != 1) {
        fail(test, "data.bin must not open as an editor tab");
        goto done;
    }
    GtkWidget *name_value = grid_value_at(binary_pages[0], 0);
    if (!name_value || !GTK_IS_LABEL(name_value) || strcmp(gtk_label_get_text(GTK_LABEL(name_value)), "data.bin") != 0) {
        fail(test, "data.bin's info tab should show Name = data.bin");
        test->failed = TRUE;
    }

    /* A directory row still just expands/collapses - no tab opens. */
    GtkTreeIter dir_iter;
    if (!find_child_by_name(model, &toolbox_iter, "subdir", &dir_iter)) {
        fail(test, "'subdir' row not found under TOOLBOX");
        goto done;
    }
    int editor_before = tabs_of_type(notebook, TAB_TYPE_EDITOR, NULL, 0);
    int binary_before = tabs_of_type(notebook, TAB_TYPE_BINARY_INFO, NULL, 0);
    GtkTreePath *dir_path = gtk_tree_model_get_path(model, &dir_iter);
    gboolean expanded_before = gtk_tree_view_row_expanded(GTK_TREE_VIEW(tree_view), dir_path);
    gtk_tree_path_free(dir_path);
    activate_row(tree_view, model, &dir_iter);
    if (tabs_of_type(notebook, TAB_TYPE_EDITOR, NULL, 0) != editor_before ||
        tabs_of_type(notebook, TAB_TYPE_BINARY_INFO, NULL, 0) != binary_before) {
        fail(test, "activating a directory row must not open any file tab");
        goto done;
    }
    GtkTreePath *dir_path2 = gtk_tree_model_get_path(model, &dir_iter);
    gboolean expanded_after = gtk_tree_view_row_expanded(GTK_TREE_VIEW(tree_view), dir_path2);
    gtk_tree_path_free(dir_path2);
    if (expanded_after == expanded_before) {
        fail(test, "activating a directory row should toggle its expanded state");
        goto done;
    }

    /* A chmod-read-only text file opens an editor tab showing the
     * read-only indicator, with its GtkTextView non-editable. */
    GtkTreeIter readonly_iter;
    if (!find_child_by_name(model, &toolbox_iter, "locked.txt", &readonly_iter)) {
        fail(test, "'locked.txt' row not found under TOOLBOX");
        goto done;
    }
    activate_row(tree_view, model, &readonly_iter);

    GtkWidget *all_editor_pages[8];
    int all_editor_count = tabs_of_type(notebook, TAB_TYPE_EDITOR, all_editor_pages, 8);
    GtkWidget *locked_page = NULL;
    for (int i = 0; i < all_editor_count; i++) {
        Tab *tab = g_object_get_data(G_OBJECT(all_editor_pages[i]), "toolbox-tab");
        if (tab && strcmp(tab->title, "locked.txt") == 0) {
            locked_page = all_editor_pages[i];
            break;
        }
    }
    if (!locked_page) {
        fail(test, "activating locked.txt should open an editor tab");
        goto done;
    }
    GtkWidget *read_only_label = g_object_get_data(G_OBJECT(locked_page), "toolbox-editor-read-only-label");
    if (!read_only_label || !gtk_widget_get_visible(read_only_label)) {
        fail(test, "locked.txt's editor tab should show a visible read-only indicator");
        test->failed = TRUE;
    }
    GtkWidget *text_view = g_object_get_data(G_OBJECT(locked_page), "toolbox-editor-text-view");
    if (!text_view || gtk_text_view_get_editable(GTK_TEXT_VIEW(text_view))) {
        fail(test, "locked.txt's GtkTextView should not be editable");
        test->failed = TRUE;
    }

    if (!test->failed) {
        test->done = TRUE;
    }

done:
    gtk_window_close(window);
    return G_SOURCE_REMOVE;
}

/* Every GTK smoke test binary lives in the same build/tests/ directory,
 * so workspace_root_init()'s exe-relative resolution finds the *same*
 * physical files/ directory for all of them - clearing any pre-existing
 * top-level entries keeps this test's TOOLBOX-child assertions correct
 * regardless of ctest run order, same precaution
 * explorer_operations_smoke.c already established. */
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

    snprintf(path, sizeof(path), "%s/notes.txt", root->canonical_path);
    write_file_bytes(path, "hello from notes\n", strlen("hello from notes\n"));

    snprintf(path, sizeof(path), "%s/data.bin", root->canonical_path);
    char nul_content[6] = {'a', 'b', '\0', 'c', 'd', 'e'};
    write_file_bytes(path, nul_content, sizeof(nul_content));

    snprintf(path, sizeof(path), "%s/subdir", root->canonical_path);
    mkdir(path, 0755);
    char nested[4400];
    snprintf(nested, sizeof(nested), "%s/subdir/inner.txt", root->canonical_path);
    write_file_bytes(nested, "inner\n", strlen("inner\n"));

    snprintf(path, sizeof(path), "%s/locked.txt", root->canonical_path);
    write_file_bytes(path, "can't touch this\n", strlen("can't touch this\n"));
    chmod(path, 0444);
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "explorer_editor_smoke: app_create failed\n");
        return 1;
    }
    test.root = *app_get_file_workspace_root(app);
    clear_workspace_root(&test.root);
    write_fixtures(&test.root);

    g_timeout_add(STEP_INTERVAL_MS, drive, &test);

    int status = app_run(app);
    app_destroy(app);

    /* Restore write permission before the next ctest run's cleanup
     * (a later test's own clear_workspace_root deletes everything under
     * files/, which needs the parent directory writable, not the file
     * itself - but being explicit here avoids leaving a chmod 444
     * fixture behind for any future manual inspection of files/). */
    char locked_path[4400];
    snprintf(locked_path, sizeof(locked_path), "%s/locked.txt", test.root.canonical_path);
    chmod(locked_path, 0644);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "explorer_editor_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "explorer_editor_smoke: test did not complete\n");
        return 1;
    }

    g_print("explorer_editor_smoke: text-file editor open, no-duplicate-tab, binary-info tab, directory "
            "expand/collapse (no tab), and read-only indicator/non-editable text view all verified\n");
    return 0;
}
