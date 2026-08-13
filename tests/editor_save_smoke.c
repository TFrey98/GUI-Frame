/*
 * Exercises Step 4's save-side checkpoint end-to-end in the real app:
 * editing a real open tab shows the modified dot and enables Save/
 * Revert; Save writes to disk and clears the dot; Save As switches the
 * tab to a new path while the old file survives untouched; Revert
 * discards an edit back to the last-saved content; and Save All across
 * several modified tabs - one of them forced to fail via a non-
 * writable parent directory - reports exactly the failing file rather
 * than silently skipping it or failing every one.
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
    fprintf(stderr, "editor_save_smoke: %s\n", msg);
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

static void activate_row(GtkWidget *tree_view, GtkTreeModel *model, GtkTreeIter *iter) {
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    GtkTreeViewColumn *column = gtk_tree_view_get_column(GTK_TREE_VIEW(tree_view), 0);
    gtk_tree_view_row_activated(GTK_TREE_VIEW(tree_view), path, column);
    gtk_tree_path_free(path);
}

static GtkWidget *find_editor_page(GtkWidget *notebook, const char *title) {
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
        if (tab && tab->type == TAB_TYPE_EDITOR && strcmp(tab->title, title) == 0) {
            return page;
        }
    }
    return NULL;
}

static void set_buffer_text(GtkWidget *page, const char *text) {
    GtkWidget *view = g_object_get_data(G_OBJECT(page), "toolbox-editor-text-view");
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buffer, text, -1);
}

static gchar *get_buffer_text(GtkWidget *page) {
    GtkWidget *view = g_object_get_data(G_OBJECT(page), "toolbox-editor-text-view");
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static gboolean label_has_dot(GtkWidget *page) {
    GtkWidget *label = g_object_get_data(G_OBJECT(page), "toolbox-tab-label-widget");
    const char *text = gtk_label_get_text(GTK_LABEL(label));
    return strstr(text, "\xE2\x97\x8F") != NULL;
}

static gboolean path_exists(const char *absolute_path) {
    return access(absolute_path, F_OK) == 0;
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

static int read_file_contents(const char *path, char *out, size_t out_size) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    size_t n = fread(out, 1, out_size - 1, f);
    out[n] = '\0';
    fclose(f);
    return 0;
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

    /* Editing a real open tab shows the modified dot and enables
     * Save/Revert. */
    GtkTreeIter alpha_iter;
    if (!find_child_by_name(model, &toolbox_iter, "alpha.txt", &alpha_iter)) {
        fail(test, "'alpha.txt' row not found under TOOLBOX");
        goto done;
    }
    activate_row(tree_view, model, &alpha_iter);
    GtkWidget *alpha_page = find_editor_page(notebook, "alpha.txt");
    if (!alpha_page) {
        fail(test, "alpha.txt's editor tab did not open");
        goto done;
    }

    set_buffer_text(alpha_page, "alpha edited\n");
    if (!label_has_dot(alpha_page)) {
        fail(test, "the modified dot should appear once alpha.txt's buffer is edited");
        goto done;
    }
    GtkWidget *save_button = g_object_get_data(G_OBJECT(alpha_page), "toolbox-editor-save-button");
    GtkWidget *revert_button = g_object_get_data(G_OBJECT(alpha_page), "toolbox-editor-revert-button");
    if (!save_button || !gtk_widget_get_sensitive(save_button)) {
        fail(test, "Save should be sensitive once alpha.txt is modified");
        goto done;
    }
    if (!revert_button || !gtk_widget_get_sensitive(revert_button)) {
        fail(test, "Revert should be sensitive once alpha.txt is modified");
        goto done;
    }

    /* Save writes to disk and clears the dot. */
    gtk_button_clicked(GTK_BUTTON(save_button));
    if (label_has_dot(alpha_page)) {
        fail(test, "the modified dot should disappear after Save");
        goto done;
    }
    char alpha_path[4400], alpha_on_disk[64] = {0};
    snprintf(alpha_path, sizeof(alpha_path), "%s/alpha.txt", test->root.canonical_path);
    read_file_contents(alpha_path, alpha_on_disk, sizeof(alpha_on_disk));
    if (strcmp(alpha_on_disk, "alpha edited\n") != 0) {
        fail(test, "alpha.txt on disk should match the saved text");
        goto done;
    }

    /* Save As switches the tab to a new path; the old file survives
     * untouched. */
    GtkWidget *save_as_button = g_object_get_data(G_OBJECT(alpha_page), "toolbox-editor-save-as-button");
    gtk_button_clicked(GTK_BUTTON(save_as_button));

    GtkWidget *save_as_dialog = NULL;
    {
        GList *toplevels = gtk_window_list_toplevels();
        for (GList *l = toplevels; l; l = l->next) {
            if (g_object_get_data(G_OBJECT(l->data), "toolbox-save-as-dialog-state")) {
                save_as_dialog = GTK_WIDGET(l->data);
                break;
            }
        }
        g_list_free(toplevels);
    }
    if (!save_as_dialog) {
        fail(test, "Save As dialog did not appear");
        goto done;
    }
    GtkWidget *path_entry = find_by_data_key(save_as_dialog, "toolbox-save-as-path-entry");
    if (!path_entry) {
        fail(test, "Save As path entry not found");
        goto done;
    }
    gtk_entry_set_text(GTK_ENTRY(path_entry), "alpha-copy.txt");
    gtk_dialog_response(GTK_DIALOG(save_as_dialog), GTK_RESPONSE_OK);

    Tab *alpha_tab = g_object_get_data(G_OBJECT(alpha_page), "toolbox-tab");
    if (strcmp(alpha_tab->title, "alpha-copy.txt") != 0) {
        fail(test, "the tab's title should switch to alpha-copy.txt after Save As");
        goto done;
    }
    if (!path_exists(alpha_path)) {
        fail(test, "the old alpha.txt file should still exist after Save As");
        goto done;
    }
    char copy_path[4400];
    snprintf(copy_path, sizeof(copy_path), "%s/alpha-copy.txt", test->root.canonical_path);
    if (!path_exists(copy_path)) {
        fail(test, "alpha-copy.txt should exist after Save As");
        goto done;
    }

    /* Revert discards an edit back to the last-saved content. */
    set_buffer_text(alpha_page, "will be discarded\n");
    if (!label_has_dot(alpha_page)) {
        fail(test, "the modified dot should reappear after editing again");
        goto done;
    }
    gtk_button_clicked(GTK_BUTTON(revert_button));
    GtkWidget *revert_dialog = find_message_dialog();
    if (!revert_dialog) {
        fail(test, "Revert should show a confirmation dialog");
        goto done;
    }
    gtk_dialog_response(GTK_DIALOG(revert_dialog), GTK_RESPONSE_YES);
    gchar *reverted = get_buffer_text(alpha_page);
    if (!reverted || strcmp(reverted, "alpha edited\n") != 0) {
        fail(test, "the buffer should revert to the last-saved content");
    }
    g_free(reverted);
    if (label_has_dot(alpha_page)) {
        fail(test, "the modified dot should be gone after Revert");
        goto done;
    }

    /* Save All across several modified tabs, one forced to fail via a
     * non-writable parent directory - reports exactly the failing one. */
    GtkTreeIter beta_iter;
    if (!find_child_by_name(model, &toolbox_iter, "beta.txt", &beta_iter)) {
        fail(test, "'beta.txt' row not found under TOOLBOX");
        goto done;
    }
    activate_row(tree_view, model, &beta_iter);
    GtkWidget *beta_page = find_editor_page(notebook, "beta.txt");
    if (!beta_page) {
        fail(test, "beta.txt's editor tab did not open");
        goto done;
    }
    set_buffer_text(beta_page, "beta edited\n");

    GtkTreeIter lockeddir_iter;
    if (!find_child_by_name(model, &toolbox_iter, "lockeddir", &lockeddir_iter)) {
        fail(test, "'lockeddir' row not found under TOOLBOX");
        goto done;
    }
    GtkTreePath *lockeddir_path_view = gtk_tree_model_get_path(model, &lockeddir_iter);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_view), lockeddir_path_view, FALSE);
    gtk_tree_path_free(lockeddir_path_view);
    GtkTreeIter gamma_iter;
    if (!find_child_by_name(model, &lockeddir_iter, "gamma.txt", &gamma_iter)) {
        fail(test, "'gamma.txt' row not found under lockeddir");
        goto done;
    }
    activate_row(tree_view, model, &gamma_iter);
    GtkWidget *gamma_page = find_editor_page(notebook, "gamma.txt");
    if (!gamma_page) {
        fail(test, "gamma.txt's editor tab did not open");
        goto done;
    }
    set_buffer_text(gamma_page, "gamma edited\n");

    char lockeddir_abs[4400];
    snprintf(lockeddir_abs, sizeof(lockeddir_abs), "%s/lockeddir", test->root.canonical_path);
    chmod(lockeddir_abs, 0555); /* no write - gamma.txt's safe-write temp file can't be created */

    GtkWidget *save_all_button = find_by_data_key(GTK_WIDGET(window), "toolbox-save-all-button");
    if (!save_all_button) {
        fail(test, "Save All button not found");
        chmod(lockeddir_abs, 0755);
        goto done;
    }
    gtk_button_clicked(GTK_BUTTON(save_all_button));
    chmod(lockeddir_abs, 0755); /* restore before this function's own cleanup below */

    if (label_has_dot(beta_page)) {
        fail(test, "beta.txt should have saved successfully via Save All");
    }
    if (!label_has_dot(gamma_page)) {
        fail(test, "gamma.txt's save should have failed and stayed modified");
    }

    GtkWidget *failure_dialog = find_message_dialog();
    if (!failure_dialog) {
        fail(test, "Save All should report gamma.txt's failure via a dialog");
        goto done;
    }
    gchar *dialog_text = NULL;
    g_object_get(G_OBJECT(failure_dialog), "text", &dialog_text, NULL);
    if (!dialog_text || !strstr(dialog_text, "gamma.txt")) {
        fail(test, "Save All's failure dialog should mention gamma.txt");
    }
    if (dialog_text && strstr(dialog_text, "beta.txt")) {
        fail(test, "Save All's failure dialog should not mention beta.txt - it saved successfully");
    }
    g_free(dialog_text);
    gtk_dialog_response(GTK_DIALOG(failure_dialog), GTK_RESPONSE_OK);

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

    snprintf(path, sizeof(path), "%s/alpha.txt", root->canonical_path);
    write_file_bytes(path, "alpha original\n", strlen("alpha original\n"));

    snprintf(path, sizeof(path), "%s/beta.txt", root->canonical_path);
    write_file_bytes(path, "beta original\n", strlen("beta original\n"));

    snprintf(path, sizeof(path), "%s/lockeddir", root->canonical_path);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/lockeddir/gamma.txt", root->canonical_path);
    write_file_bytes(path, "gamma original\n", strlen("gamma original\n"));
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "editor_save_smoke: app_create failed\n");
        return 1;
    }
    test.root = *app_get_file_workspace_root(app);
    clear_workspace_root(&test.root);
    write_fixtures(&test.root);

    g_timeout_add(STEP_INTERVAL_MS, drive, &test);

    int status = app_run(app);
    app_destroy(app);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "editor_save_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "editor_save_smoke: test did not complete\n");
        return 1;
    }

    g_print("editor_save_smoke: modified dot, Save, Save As, Revert, and Save All (with a reported per-file "
            "failure) all verified\n");
    return 0;
}
