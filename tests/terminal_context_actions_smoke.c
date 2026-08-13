/*
 * Exercises Step 5's non-spawn-heavy checkpoint end-to-end in the real
 * app: a folder's "Open in Integrated Terminal" opens a new terminal
 * tab rooted at the folder's real path; an ordinary file's "Open in
 * Terminal Directory" roots one at its parent instead; "Open as Text"
 * is offered (and opens the same editor tab double-click would) for a
 * text script, and absent for a real binary executable; "Copy Path"/
 * "Copy Relative Path" put the expected strings on the clipboard; and
 * running a script via "Run in Terminal" doesn't touch an already-open
 * editor tab for that same script - it stays open and editable.
 */
#include <dirent.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/app.h"
#include "core/tab.h"
#include "core/terminal_session.h"
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
    fprintf(stderr, "terminal_context_actions_smoke: %s\n", msg);
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
    return g_object_get_data(G_OBJECT(tree_view), "workbench-explorer-context-menu");
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

static int count_terminal_pages(GtkWidget *notebook) {
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    int count = 0;
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
        if (tab && tab->type == TAB_TYPE_TERMINAL) {
            count++;
        }
    }
    return count;
}

/* The most-recently-appended TAB_TYPE_TERMINAL page - new tabs are
 * always appended, so the highest matching notebook index is the
 * newest one. */
static GtkWidget *newest_terminal_page(GtkWidget *notebook) {
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    GtkWidget *newest = NULL;
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
        if (tab && tab->type == TAB_TYPE_TERMINAL) {
            newest = page;
        }
    }
    return newest;
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
    GtkWidget *tree_view = window ? find_by_data_key(GTK_WIDGET(window), "workbench-explorer-tree") : NULL;
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
                gtk_widget_destroy(GTK_WIDGET(window));
            }
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));
    GtkTreeIter workbench_iter;
    if (!gtk_tree_model_get_iter_first(model, &workbench_iter) || !row_name_is(model, &workbench_iter, "TOOLBOX")) {
        fail(test, "expected TOOLBOX as the first top-level row");
        goto done;
    }

    /* "Open in Integrated Terminal" on a folder roots a new terminal
     * tab at the folder's real path. */
    GtkTreeIter folder_iter;
    if (!find_child_by_name(model, &workbench_iter, "folder1", &folder_iter)) {
        fail(test, "'folder1' row not found under TOOLBOX");
        goto done;
    }
    int terminals_before = count_terminal_pages(notebook);
    GtkWidget *menu = open_menu_for_row(tree_view, model, &folder_iter);
    if (!menu || !click_menu_item(menu, "Open in Integrated Terminal")) {
        fail(test, "could not click 'Open in Integrated Terminal' on folder1");
        goto done;
    }
    if (count_terminal_pages(notebook) != terminals_before + 1) {
        fail(test, "'Open in Integrated Terminal' should open exactly one new terminal tab");
        goto done;
    }
    GtkWidget *folder_terminal_page = newest_terminal_page(notebook);
    Tab *folder_terminal_tab = g_object_get_data(G_OBJECT(folder_terminal_page), "workbench-tab");
    TerminalSession *folder_session = folder_terminal_tab->backend_data;
    char expected_folder_path[4400];
    snprintf(expected_folder_path, sizeof(expected_folder_path), "%s/folder1", test->root.canonical_path);
    if (strcmp(folder_session->working_directory, expected_folder_path) != 0) {
        fail(test, "the new terminal's working directory should be folder1's real path");
        goto done;
    }

    /* "Open in Terminal Directory" on an ordinary file roots a new
     * terminal tab at its parent (TOOLBOX itself, here). */
    GtkTreeIter plainfile_iter;
    if (!find_child_by_name(model, &workbench_iter, "plainfile.txt", &plainfile_iter)) {
        fail(test, "'plainfile.txt' row not found under TOOLBOX");
        goto done;
    }
    terminals_before = count_terminal_pages(notebook);
    menu = open_menu_for_row(tree_view, model, &plainfile_iter);
    if (!menu || !click_menu_item(menu, "Open in Terminal Directory")) {
        fail(test, "could not click 'Open in Terminal Directory' on plainfile.txt");
        goto done;
    }
    if (count_terminal_pages(notebook) != terminals_before + 1) {
        fail(test, "'Open in Terminal Directory' should open exactly one new terminal tab");
        goto done;
    }
    GtkWidget *file_terminal_page = newest_terminal_page(notebook);
    Tab *file_terminal_tab = g_object_get_data(G_OBJECT(file_terminal_page), "workbench-tab");
    TerminalSession *file_session = file_terminal_tab->backend_data;
    if (strcmp(file_session->working_directory, test->root.canonical_path) != 0) {
        fail(test, "plainfile.txt's terminal should be rooted at TOOLBOX (its parent)");
        goto done;
    }

    /* "Open as Text" is offered for a text script and opens the same
     * editor tab double-click would; absent for a real binary
     * executable. */
    GtkTreeIter script_iter;
    if (!find_child_by_name(model, &workbench_iter, "script.sh", &script_iter)) {
        fail(test, "'script.sh' row not found under TOOLBOX");
        goto done;
    }
    menu = open_menu_for_row(tree_view, model, &script_iter);
    if (!menu || !find_menu_item(menu, "Open as Text")) {
        fail(test, "'Open as Text' should be offered for a text script");
        if (menu) {
            gtk_menu_popdown(GTK_MENU(menu));
        }
        goto done;
    }
    if (!click_menu_item(menu, "Open as Text")) {
        fail(test, "could not click 'Open as Text' on script.sh");
        goto done;
    }
    GtkWidget *script_editor_page = find_editor_page(notebook, "script.sh");
    if (!script_editor_page) {
        fail(test, "'Open as Text' should open script.sh as an editor tab");
        goto done;
    }

    GtkTreeIter binary_iter;
    if (!find_child_by_name(model, &workbench_iter, "binary_exe", &binary_iter)) {
        fail(test, "'binary_exe' row not found under TOOLBOX");
        goto done;
    }
    menu = open_menu_for_row(tree_view, model, &binary_iter);
    if (!menu) {
        fail(test, "binary_exe's context menu did not appear");
        goto done;
    }
    if (find_menu_item(menu, "Open as Text")) {
        fail(test, "'Open as Text' should not be offered for a real binary executable");
    }
    if (!find_menu_item(menu, "Run in Terminal") || !find_menu_item(menu, "Run with Arguments\xE2\x80\xA6")) {
        fail(test, "binary_exe should still offer Run in Terminal/Run with Arguments...");
    }
    gtk_menu_popdown(GTK_MENU(menu));

    /* Copy Path / Copy Relative Path put the expected strings on the
     * clipboard. */
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    menu = open_menu_for_row(tree_view, model, &folder_iter);
    if (!menu || !click_menu_item(menu, "Copy Path")) {
        fail(test, "could not click 'Copy Path' on folder1");
        goto done;
    }
    gchar *copied_path = gtk_clipboard_wait_for_text(clipboard);
    if (!copied_path || strcmp(copied_path, expected_folder_path) != 0) {
        fail(test, "'Copy Path' should copy folder1's real resolved path");
    }
    g_free(copied_path);

    menu = open_menu_for_row(tree_view, model, &folder_iter);
    if (!menu || !click_menu_item(menu, "Copy Relative Path")) {
        fail(test, "could not click 'Copy Relative Path' on folder1");
        goto done;
    }
    gchar *copied_relative = gtk_clipboard_wait_for_text(clipboard);
    if (!copied_relative || strcmp(copied_relative, "folder1") != 0) {
        fail(test, "'Copy Relative Path' should copy folder1's relative path");
    }
    g_free(copied_relative);

    /* Running a script via "Run in Terminal" doesn't touch an
     * already-open editor tab for that same script - it stays open and
     * editable. */
    menu = open_menu_for_row(tree_view, model, &script_iter);
    if (!menu || !click_menu_item(menu, "Run in Terminal")) {
        fail(test, "could not click 'Run in Terminal' on script.sh");
        goto done;
    }
    if (!find_editor_page(notebook, "script.sh")) {
        fail(test, "script.sh's editor tab should remain open after Run in Terminal");
        goto done;
    }
    GtkWidget *script_view = g_object_get_data(G_OBJECT(script_editor_page), "workbench-editor-text-view");
    if (!script_view || !gtk_text_view_get_editable(GTK_TEXT_VIEW(script_view))) {
        fail(test, "script.sh should remain editable after being run");
    }

    if (!test->failed) {
        test->done = TRUE;
    }

done:
    if (window) {
        gtk_widget_destroy(GTK_WIDGET(window));
    }
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

    snprintf(path, sizeof(path), "%s/folder1", root->canonical_path);
    mkdir(path, 0755);

    snprintf(path, sizeof(path), "%s/plainfile.txt", root->canonical_path);
    write_file_bytes(path, "plain\n", strlen("plain\n"));

    snprintf(path, sizeof(path), "%s/script.sh", root->canonical_path);
    const char *script = "#!/bin/sh\necho hello\n";
    write_file_bytes(path, script, strlen(script));
    chmod(path, 0755);

    snprintf(path, sizeof(path), "%s/binary_exe", root->canonical_path);
    char binary_content[8] = {'\x7f', 'E', 'L', 'F', '\0', 'x', 'y', 'z'};
    write_file_bytes(path, binary_content, sizeof(binary_content));
    chmod(path, 0755);
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "terminal_context_actions_smoke: app_create failed\n");
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
        fprintf(stderr, "terminal_context_actions_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "terminal_context_actions_smoke: test did not complete\n");
        return 1;
    }

    g_print("terminal_context_actions_smoke: Open in Integrated Terminal, Open in Terminal Directory, Open as "
            "Text presence/absence, Copy Path/Copy Relative Path, and script-remains-editable-after-run all "
            "verified\n");
    return 0;
}
