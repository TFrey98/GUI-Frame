/*
 * Exercises full parity between the explorer's "Toolkit" section and
 * its "TOOLBOX" section, reported directly by the user as a bug
 * ("anything that populates in the file explorer panel should be able
 * to be interacted with"): New Folder/Rename/Delete via Toolkit's own
 * context menu actually write to toolkit/; double-click opens a text
 * fixture as an editor tab and a binary fixture as binary-info;
 * Properties reports correct fields; Copy Path/Copy Relative Path copy
 * the right strings; Open in Integrated Terminal roots a new terminal
 * at the real toolkit/ path; Run in Terminal is offered and spawns; and
 * a FILES-sourced and a Toolkit-sourced tab for files that happen to
 * share the same relative path stay genuinely independent (proving
 * find_file_tab()'s root-aware matching - the collision
 * EditorDocument.root exists to prevent).
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
    return windows ? GTK_WINDOW(windows->data) : NULL;
}

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "toolkit_interaction_smoke: %s\n", msg);
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

static gboolean find_pending_child(GtkTreeModel *model, GtkTreeIter *parent, GtkTreeIter *out) {
    GtkTreeIter child;
    if (!gtk_tree_model_iter_children(model, &child, parent)) {
        return FALSE;
    }
    do {
        guint64 node_id = 0;
        gtk_tree_model_get(model, &child, EXPLORER_COL_NODE_ID, &node_id, -1);
        if (node_id == (guint64)0xFFFFFFFFFFFFFFFFULL) {
            *out = child;
            return TRUE;
        }
    } while (gtk_tree_model_iter_next(model, &child));
    return FALSE;
}

static GtkCellRenderer *find_name_renderer(GtkWidget *tree_view) {
    GtkTreeViewColumn *column = gtk_tree_view_get_column(GTK_TREE_VIEW(tree_view), 0);
    if (!column) {
        return NULL;
    }
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(column));
    GtkCellRenderer *result = NULL;
    for (GList *l = cells; l; l = l->next) {
        if (GTK_IS_CELL_RENDERER_TEXT(l->data)) {
            result = GTK_CELL_RENDERER(l->data);
            break;
        }
    }
    g_list_free(cells);
    return result;
}

static void commit_row(GtkWidget *tree_view, GtkTreeModel *model, GtkTreeIter *iter, const char *text) {
    GtkCellRenderer *renderer = find_name_renderer(tree_view);
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gchar *path_str = gtk_tree_path_to_string(path);
    gtk_tree_path_free(path);
    g_signal_emit_by_name(renderer, "edited", path_str, text);
    g_free(path_str);
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

static gboolean path_exists(const char *absolute_path) {
    return access(absolute_path, F_OK) == 0;
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

static int count_pages_of_type(GtkWidget *notebook, TabType type) {
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    int count = 0;
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
        if (tab && tab->type == type) {
            count++;
        }
    }
    return count;
}

static GtkWidget *newest_page_of_type(GtkWidget *notebook, TabType type) {
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    GtkWidget *newest = NULL;
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
        if (tab && tab->type == type) {
            newest = page;
        }
    }
    return newest;
}

static gchar *editor_buffer_text(GtkWidget *page) {
    GtkWidget *view = g_object_get_data(G_OBJECT(page), "workbench-editor-text-view");
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static GtkWidget *grid_value_at(GtkWidget *widget, int row) {
    GPtrArray *grids = g_ptr_array_new();
    collect_by_type(widget, grids, GTK_TYPE_GRID);
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
    GtkTreeIter workbench_iter, toolkit_iter;
    if (!gtk_tree_model_get_iter_first(model, &workbench_iter) || !row_name_is(model, &workbench_iter, "TOOLBOX")) {
        fail(test, "expected TOOLBOX as the first top-level row");
        goto done;
    }
    toolkit_iter = workbench_iter;
    if (!gtk_tree_model_iter_next(model, &toolkit_iter) || !row_name_is(model, &toolkit_iter, "Toolkit")) {
        fail(test, "expected Toolkit as the second top-level row");
        goto done;
    }

    /* New Folder via Toolkit's own context menu actually writes to
     * toolkit/. */
    GtkWidget *menu = open_menu_for_row(tree_view, model, &toolkit_iter);
    if (!menu || !click_menu_item(menu, "New Folder")) {
        fail(test, "could not click 'New Folder' on the Toolkit root");
        goto done;
    }
    GtkTreeIter pending;
    if (!find_pending_child(model, &toolkit_iter, &pending)) {
        fail(test, "no pending blank row appeared under Toolkit after New Folder");
        goto done;
    }
    commit_row(tree_view, model, &pending, "newtool");
    char newtool_path[4400];
    snprintf(newtool_path, sizeof(newtool_path), "%s/newtool", test->toolkit_root.canonical_path);
    if (!path_exists(newtool_path)) {
        fail(test, "'newtool' was not created on disk under toolkit/");
        goto done;
    }
    GtkTreeIter newtool_iter;
    if (!find_child_by_name(model, &toolkit_iter, "newtool", &newtool_iter)) {
        fail(test, "'newtool' row not found under Toolkit");
        goto done;
    }

    /* Rename it via context menu. */
    menu = open_menu_for_row(tree_view, model, &newtool_iter);
    if (!menu || !click_menu_item(menu, "Rename")) {
        fail(test, "could not click 'Rename' on newtool's context menu");
        goto done;
    }
    commit_row(tree_view, model, &newtool_iter, "renamedtool");
    char renamedtool_path[4400];
    snprintf(renamedtool_path, sizeof(renamedtool_path), "%s/renamedtool", test->toolkit_root.canonical_path);
    if (path_exists(newtool_path) || !path_exists(renamedtool_path)) {
        fail(test, "renaming newtool -> renamedtool did not take effect on disk");
        goto done;
    }
    GtkTreeIter renamedtool_iter;
    if (!find_child_by_name(model, &toolkit_iter, "renamedtool", &renamedtool_iter)) {
        fail(test, "'renamedtool' row not found under Toolkit after rename");
        goto done;
    }

    /* Delete it (empty folder - immediate, no confirmation). */
    menu = open_menu_for_row(tree_view, model, &renamedtool_iter);
    if (!menu || !click_menu_item(menu, "Delete")) {
        fail(test, "could not click 'Delete' on renamedtool's context menu");
        goto done;
    }
    if (path_exists(renamedtool_path)) {
        fail(test, "'renamedtool' should be gone immediately after Delete");
        goto done;
    }

    /* Double-click opens a text fixture as an editor tab. */
    GtkTreeIter existing_iter;
    if (!find_child_by_name(model, &toolkit_iter, "existing.txt", &existing_iter)) {
        fail(test, "'existing.txt' row not found under Toolkit");
        goto done;
    }
    GtkTreePath *existing_path = gtk_tree_model_get_path(model, &existing_iter);
    gtk_tree_view_row_activated(GTK_TREE_VIEW(tree_view), existing_path,
                                 gtk_tree_view_get_column(GTK_TREE_VIEW(tree_view), 0));
    gtk_tree_path_free(existing_path);
    GtkWidget *existing_page = find_editor_page(notebook, "existing.txt");
    if (!existing_page) {
        fail(test, "'existing.txt' did not open as an editor tab");
        goto done;
    }
    gchar *existing_text = editor_buffer_text(existing_page);
    if (!existing_text || strcmp(existing_text, "toolkit existing content\n") != 0) {
        fail(test, "'existing.txt' editor tab should show the file's real content");
    }
    g_free(existing_text);

    /* Double-click opens a binary fixture as binary-info. */
    GtkTreeIter binary_iter;
    if (!find_child_by_name(model, &toolkit_iter, "binary.dat", &binary_iter)) {
        fail(test, "'binary.dat' row not found under Toolkit");
        goto done;
    }
    GtkTreePath *binary_path = gtk_tree_model_get_path(model, &binary_iter);
    gtk_tree_view_row_activated(GTK_TREE_VIEW(tree_view), binary_path,
                                 gtk_tree_view_get_column(GTK_TREE_VIEW(tree_view), 0));
    gtk_tree_path_free(binary_path);
    if (count_pages_of_type(notebook, TAB_TYPE_BINARY_INFO) != 1) {
        fail(test, "'binary.dat' should have opened exactly one binary-info tab");
        goto done;
    }

    /* Properties reports correct fields (script.sh is executable). */
    GtkTreeIter script_iter;
    if (!find_child_by_name(model, &toolkit_iter, "script.sh", &script_iter)) {
        fail(test, "'script.sh' row not found under Toolkit");
        goto done;
    }
    menu = open_menu_for_row(tree_view, model, &script_iter);
    if (!menu || !click_menu_item(menu, "Properties")) {
        fail(test, "could not click 'Properties' on script.sh");
        goto done;
    }
    GtkWidget *properties_dialog = NULL;
    {
        GList *toplevels = gtk_window_list_toplevels();
        for (GList *l = toplevels; l; l = l->next) {
            if (GTK_IS_DIALOG(l->data) && gtk_window_get_title(GTK_WINDOW(l->data)) &&
                strcmp(gtk_window_get_title(GTK_WINDOW(l->data)), "Properties") == 0) {
                properties_dialog = GTK_WIDGET(l->data);
                break;
            }
        }
        g_list_free(toplevels);
    }
    if (!properties_dialog) {
        fail(test, "Properties dialog did not appear for script.sh");
        goto done;
    }
    GtkWidget *executable_value = grid_value_at(properties_dialog, 4);
    if (!executable_value || !GTK_IS_LABEL(executable_value) ||
        strcmp(gtk_label_get_text(GTK_LABEL(executable_value)), "Yes") != 0) {
        fail(test, "Properties should report script.sh as Executable = Yes");
    }
    gtk_widget_destroy(properties_dialog);

    /* Copy Path / Copy Relative Path copy the right strings. */
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    menu = open_menu_for_row(tree_view, model, &existing_iter);
    if (!menu || !click_menu_item(menu, "Copy Path")) {
        fail(test, "could not click 'Copy Path' on existing.txt");
        goto done;
    }
    gchar *copied_path = gtk_clipboard_wait_for_text(clipboard);
    char expected_path[4400];
    snprintf(expected_path, sizeof(expected_path), "%s/existing.txt", test->toolkit_root.canonical_path);
    if (!copied_path || strcmp(copied_path, expected_path) != 0) {
        fail(test, "'Copy Path' should copy existing.txt's real resolved toolkit path");
    }
    g_free(copied_path);

    menu = open_menu_for_row(tree_view, model, &existing_iter);
    if (!menu || !click_menu_item(menu, "Copy Relative Path")) {
        fail(test, "could not click 'Copy Relative Path' on existing.txt");
        goto done;
    }
    gchar *copied_relative = gtk_clipboard_wait_for_text(clipboard);
    if (!copied_relative || strcmp(copied_relative, "existing.txt") != 0) {
        fail(test, "'Copy Relative Path' should copy 'existing.txt'");
    }
    g_free(copied_relative);

    /* Open in Integrated Terminal (on the Toolkit root) roots a new
     * terminal tab at the real toolkit/ path. */
    int terminals_before = count_pages_of_type(notebook, TAB_TYPE_TERMINAL);
    menu = open_menu_for_row(tree_view, model, &toolkit_iter);
    if (!menu || !click_menu_item(menu, "Open in Integrated Terminal")) {
        fail(test, "could not click 'Open in Integrated Terminal' on the Toolkit root");
        goto done;
    }
    if (count_pages_of_type(notebook, TAB_TYPE_TERMINAL) != terminals_before + 1) {
        fail(test, "'Open in Integrated Terminal' should open exactly one new terminal tab");
        goto done;
    }
    GtkWidget *toolkit_terminal_page = newest_page_of_type(notebook, TAB_TYPE_TERMINAL);
    Tab *toolkit_terminal_tab = g_object_get_data(G_OBJECT(toolkit_terminal_page), "workbench-tab");
    TerminalSession *toolkit_session = toolkit_terminal_tab->backend_data;
    if (strcmp(toolkit_session->working_directory, test->toolkit_root.canonical_path) != 0) {
        fail(test, "the new terminal's working directory should be toolkit/'s real path");
    }

    /* Run in Terminal is offered for an executable Toolkit row and
     * spawns a new terminal (terminal_run_command_smoke.c already
     * proves argv/cwd/env actually reach the process - this just
     * proves Toolkit rows can trigger it at all). */
    terminals_before = count_pages_of_type(notebook, TAB_TYPE_TERMINAL);
    menu = open_menu_for_row(tree_view, model, &script_iter);
    if (!menu || !find_menu_item(menu, "Run in Terminal")) {
        fail(test, "'Run in Terminal' should be offered for script.sh");
        if (menu) {
            gtk_menu_popdown(GTK_MENU(menu));
        }
        goto done;
    }
    if (!click_menu_item(menu, "Run in Terminal")) {
        fail(test, "could not click 'Run in Terminal' on script.sh");
        goto done;
    }
    if (count_pages_of_type(notebook, TAB_TYPE_TERMINAL) != terminals_before + 1) {
        fail(test, "'Run in Terminal' should open exactly one new terminal tab");
    }

    /* A FILES-sourced and a Toolkit-sourced file that happen to share
     * the same relative path stay genuinely independent tabs - proving
     * find_file_tab()'s root-aware matching. */
    GtkTreeIter files_samename_iter, toolkit_samename_iter;
    if (!find_child_by_name(model, &workbench_iter, "samename.txt", &files_samename_iter) ||
        !find_child_by_name(model, &toolkit_iter, "samename.txt", &toolkit_samename_iter)) {
        fail(test, "'samename.txt' row not found under both TOOLBOX and Toolkit");
        goto done;
    }
    GtkTreePath *files_samename_path = gtk_tree_model_get_path(model, &files_samename_iter);
    gtk_tree_view_row_activated(GTK_TREE_VIEW(tree_view), files_samename_path,
                                 gtk_tree_view_get_column(GTK_TREE_VIEW(tree_view), 0));
    gtk_tree_path_free(files_samename_path);
    GtkTreePath *toolkit_samename_path = gtk_tree_model_get_path(model, &toolkit_samename_iter);
    gtk_tree_view_row_activated(GTK_TREE_VIEW(tree_view), toolkit_samename_path,
                                 gtk_tree_view_get_column(GTK_TREE_VIEW(tree_view), 0));
    gtk_tree_path_free(toolkit_samename_path);

    if (count_pages_of_type(notebook, TAB_TYPE_EDITOR) < 2) {
        fail(test, "both samename.txt files should have opened as separate editor tabs");
        goto done;
    }
    int samename_editor_count = 0;
    gboolean saw_files_content = FALSE, saw_toolkit_content = FALSE;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
        if (!tab || tab->type != TAB_TYPE_EDITOR || strcmp(tab->title, "samename.txt") != 0) {
            continue;
        }
        samename_editor_count++;
        gchar *content = editor_buffer_text(page);
        if (content && strcmp(content, "files samename\n") == 0) {
            saw_files_content = TRUE;
        } else if (content && strcmp(content, "toolkit samename\n") == 0) {
            saw_toolkit_content = TRUE;
        }
        g_free(content);
    }
    if (samename_editor_count != 2) {
        fail(test, "expected exactly two independent samename.txt editor tabs (FILES and Toolkit)");
    }
    if (!saw_files_content || !saw_toolkit_content) {
        fail(test, "the two samename.txt tabs should each show their own root's distinct content");
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
 * so workspace_root_init()'s (and toolkit_index's) exe-relative
 * resolution finds the *same* physical files/ and toolkit/ directories
 * for all of them - clearing any pre-existing top-level entries from
 * both keeps this test's assertions correct regardless of ctest run
 * order, same precaution every explorer/terminal smoke test already
 * established for files/, now extended to toolkit/ too. */
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

static void write_fixtures(const WorkspaceRoot *files_root, const WorkspaceRoot *toolkit_root) {
    char path[4400];

    snprintf(path, sizeof(path), "%s/existing.txt", toolkit_root->canonical_path);
    write_file_bytes(path, "toolkit existing content\n", strlen("toolkit existing content\n"));

    snprintf(path, sizeof(path), "%s/script.sh", toolkit_root->canonical_path);
    const char *script = "#!/bin/sh\necho hi\n";
    write_file_bytes(path, script, strlen(script));
    chmod(path, 0755);

    snprintf(path, sizeof(path), "%s/binary.dat", toolkit_root->canonical_path);
    char binary_content[6] = {'a', 'b', '\0', 'c', 'd', 'e'};
    write_file_bytes(path, binary_content, sizeof(binary_content));

    snprintf(path, sizeof(path), "%s/samename.txt", toolkit_root->canonical_path);
    write_file_bytes(path, "toolkit samename\n", strlen("toolkit samename\n"));

    snprintf(path, sizeof(path), "%s/samename.txt", files_root->canonical_path);
    write_file_bytes(path, "files samename\n", strlen("files samename\n"));
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "toolkit_interaction_smoke: app_create failed\n");
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
        fprintf(stderr, "toolkit_interaction_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "toolkit_interaction_smoke: test did not complete\n");
        return 1;
    }

    g_print("toolkit_interaction_smoke: Toolkit New Folder/Rename/Delete, double-click open (text + binary), "
            "Properties, Copy Path/Copy Relative Path, Open in Integrated Terminal, Run in Terminal, and "
            "FILES/Toolkit tab independence all verified\n");
    return 0;
}
