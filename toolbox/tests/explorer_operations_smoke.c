/*
 * Exercises Step 2's checkpoint end-to-end in the real app: toolbar
 * New Folder, a folder's context-menu New File, Rename, guarded Delete
 * (immediate for an ordinary file, confirmed for a non-empty folder),
 * Refresh preserving an expanded folder's state, and that the TOOLBOX
 * root's own context menu never offers Rename/Delete.
 *
 * Commits an inline create/rename by emitting the name cell renderer's
 * own "edited" signal directly, the same technique context_menu_smoke.c
 * already established for driving a context menu via "popup-menu" -
 * a real, standard GTK signal, not simulated raw input. This is a
 * deliberate choice, not just a style preference: this environment's X
 * server has no window manager assigning real input focus, so
 * interactive cell editing (which depends on the toplevel actually
 * being focused for a child widget's grab_focus to stick) never
 * visibly starts here, even though the row append and edit-start calls
 * both succeed - matching the window-focus/position instability
 * already noted in this suite's manual-testing sections since Phase 7.
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

/* Mirrors ui_gtk.c's file-local sentinel - not a public API. */
#define EXPLORER_PENDING_CREATE_ID ((guint64)0xFFFFFFFFFFFFFFFFULL)

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
    fprintf(stderr, "explorer_operations_smoke: %s\n", msg);
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
        if (node_id == EXPLORER_PENDING_CREATE_ID) {
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

/* Commits iter's row by emitting the name renderer's own "edited"
 * signal - see the file comment for why this bypasses the (here,
 * non-functional) interactive GtkEntry entirely. */
static void commit_row(GtkWidget *tree_view, GtkTreeModel *model, GtkTreeIter *iter, const char *text) {
    GtkCellRenderer *renderer = find_name_renderer(tree_view);
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gchar *path_str = gtk_tree_path_to_string(path);
    gtk_tree_path_free(path);
    g_signal_emit_by_name(renderer, "edited", path_str, text);
    g_free(path_str);
}

/* "popup-menu" is a real, standard GTK signal (like row-activated) -
 * emitting it programmatically is the keyboard-context-menu trigger GTK
 * itself defines (e.g. Shift+F10), same technique context_menu_smoke.c
 * already established for the object panel's own menu. */
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
    /* Activating an item this way (no real button-release event behind
     * the popup - see the "no trigger event for menu popup" warning)
     * doesn't reliably release GTK's own popup grab on its own; leaving
     * it held blocks the window from closing at the end of the test. */
    gtk_menu_popdown(GTK_MENU(menu));
    return TRUE;
}

static gboolean path_exists(const char *absolute_path) {
    return access(absolute_path, F_OK) == 0;
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
    char path_buf[4400];

    GtkTreeIter toolbox_iter;
    if (!gtk_tree_model_get_iter_first(model, &toolbox_iter) || !row_name_is(model, &toolbox_iter, "TOOLBOX")) {
        fail(test, "expected TOOLBOX as the first top-level row");
        goto done;
    }

    /* Toolbar New Folder, nothing selected -> targets TOOLBOX. */
    GtkWidget *new_folder_button = find_by_data_key(GTK_WIDGET(window), "toolbox-explorer-new-folder-button");
    if (!new_folder_button) {
        fail(test, "New Folder toolbar button not found");
        goto done;
    }
    gtk_button_clicked(GTK_BUTTON(new_folder_button));
    GtkTreeIter pending;
    if (!find_pending_child(model, &toolbox_iter, &pending)) {
        fail(test, "no pending blank row appeared under TOOLBOX after New Folder");
        goto done;
    }
    commit_row(tree_view, model, &pending, "newfolder");
    snprintf(path_buf, sizeof(path_buf), "%s/newfolder", test->root.canonical_path);
    if (!path_exists(path_buf)) {
        fail(test, "'newfolder' was not created on disk");
        goto done;
    }
    GtkTreeIter newfolder_iter;
    if (!find_child_by_name(model, &toolbox_iter, "newfolder", &newfolder_iter)) {
        fail(test, "'newfolder' row not found under TOOLBOX");
        goto done;
    }

    /* Folder context menu -> New File, inside newfolder. */
    GtkWidget *menu = open_menu_for_row(tree_view, model, &newfolder_iter);
    if (!menu || !click_menu_item(menu, "New File")) {
        fail(test, "could not click 'New File' on newfolder's context menu");
        goto done;
    }
    if (!find_pending_child(model, &newfolder_iter, &pending)) {
        fail(test, "no pending blank row appeared under newfolder after New File");
        goto done;
    }
    commit_row(tree_view, model, &pending, "newfile.txt");
    snprintf(path_buf, sizeof(path_buf), "%s/newfolder/newfile.txt", test->root.canonical_path);
    if (!path_exists(path_buf)) {
        fail(test, "'newfolder/newfile.txt' was not created on disk");
        goto done;
    }
    /* Creating a child replaced newfolder's own row via load_row_children -
     * re-fetch it by name (ids aren't stable across a reload). */
    if (!find_child_by_name(model, &toolbox_iter, "newfolder", &newfolder_iter)) {
        fail(test, "'newfolder' row missing after New File");
        goto done;
    }
    GtkTreeIter newfile_iter;
    if (!find_child_by_name(model, &newfolder_iter, "newfile.txt", &newfile_iter)) {
        fail(test, "'newfile.txt' row not found under newfolder");
        goto done;
    }

    /* Rename newfile.txt -> renamed.txt. */
    menu = open_menu_for_row(tree_view, model, &newfile_iter);
    if (!menu || !click_menu_item(menu, "Rename")) {
        fail(test, "could not click 'Rename' on newfile.txt's context menu");
        goto done;
    }
    commit_row(tree_view, model, &newfile_iter, "renamed.txt");
    snprintf(path_buf, sizeof(path_buf), "%s/newfolder/newfile.txt", test->root.canonical_path);
    if (path_exists(path_buf)) {
        fail(test, "old name 'newfolder/newfile.txt' should be gone after rename");
        goto done;
    }
    snprintf(path_buf, sizeof(path_buf), "%s/newfolder/renamed.txt", test->root.canonical_path);
    if (!path_exists(path_buf)) {
        fail(test, "'newfolder/renamed.txt' should exist after rename");
        goto done;
    }
    if (!find_child_by_name(model, &toolbox_iter, "newfolder", &newfolder_iter) ||
        !find_child_by_name(model, &newfolder_iter, "renamed.txt", &newfile_iter)) {
        fail(test, "'renamed.txt' row not found under newfolder after rename");
        goto done;
    }

    /* Delete an ordinary (non-executable) file - immediate, no dialog. */
    menu = open_menu_for_row(tree_view, model, &newfile_iter);
    if (!menu || !click_menu_item(menu, "Delete")) {
        fail(test, "could not click 'Delete' on renamed.txt's context menu");
        goto done;
    }
    if (path_exists(path_buf)) {
        fail(test, "'newfolder/renamed.txt' should be gone immediately (no confirmation needed)");
        goto done;
    }

    /* Create a file inside newfolder to make it non-empty again, then
     * try to delete newfolder itself - this must require confirmation. */
    if (!find_child_by_name(model, &toolbox_iter, "newfolder", &newfolder_iter)) {
        fail(test, "'newfolder' row missing before non-empty-delete setup");
        goto done;
    }
    menu = open_menu_for_row(tree_view, model, &newfolder_iter);
    if (!menu || !click_menu_item(menu, "New File")) {
        fail(test, "could not click 'New File' to repopulate newfolder");
        goto done;
    }
    if (!find_pending_child(model, &newfolder_iter, &pending)) {
        fail(test, "no pending blank row appeared under newfolder for keepme.txt");
        goto done;
    }
    commit_row(tree_view, model, &pending, "keepme.txt");
    if (!find_child_by_name(model, &toolbox_iter, "newfolder", &newfolder_iter)) {
        fail(test, "'newfolder' row missing after repopulating it");
        goto done;
    }

    menu = open_menu_for_row(tree_view, model, &newfolder_iter);
    if (!menu || !click_menu_item(menu, "Delete")) {
        fail(test, "could not click 'Delete' on non-empty newfolder");
        goto done;
    }
    GtkWidget *confirm_dialog = NULL;
    GList *toplevels = gtk_window_list_toplevels();
    for (GList *l = toplevels; l; l = l->next) {
        if (GTK_IS_MESSAGE_DIALOG(l->data)) {
            confirm_dialog = GTK_WIDGET(l->data);
            break;
        }
    }
    g_list_free(toplevels);
    if (!confirm_dialog) {
        fail(test, "expected a confirmation dialog deleting a non-empty folder");
        goto done;
    }
    gtk_dialog_response(GTK_DIALOG(confirm_dialog), GTK_RESPONSE_YES);
    snprintf(path_buf, sizeof(path_buf), "%s/newfolder", test->root.canonical_path);
    if (path_exists(path_buf)) {
        fail(test, "'newfolder' should be gone after confirming delete");
        goto done;
    }

    /* Refresh preserves an already-expanded folder's state. Toolbar New
     * Folder again, nothing selected -> targets TOOLBOX. */
    GtkWidget *new_folder_button2 = find_by_data_key(GTK_WIDGET(window), "toolbox-explorer-new-folder-button");
    if (!new_folder_button2) {
        fail(test, "New Folder toolbar button not found");
        goto done;
    }
    gtk_button_clicked(GTK_BUTTON(new_folder_button2));
    if (!find_pending_child(model, &toolbox_iter, &pending)) {
        fail(test, "no pending blank row appeared under TOOLBOX for expandedfolder");
        goto done;
    }
    commit_row(tree_view, model, &pending, "expandedfolder");
    GtkTreeIter expanded_folder_iter;
    if (!find_child_by_name(model, &toolbox_iter, "expandedfolder", &expanded_folder_iter)) {
        fail(test, "'expandedfolder' row not found under TOOLBOX");
        goto done;
    }
    menu = open_menu_for_row(tree_view, model, &expanded_folder_iter);
    if (!menu || !click_menu_item(menu, "New File")) {
        fail(test, "could not click 'New File' inside expandedfolder");
        goto done;
    }
    if (!find_pending_child(model, &expanded_folder_iter, &pending)) {
        fail(test, "no pending blank row appeared under expandedfolder for child.txt");
        goto done;
    }
    commit_row(tree_view, model, &pending, "child.txt");
    if (!find_child_by_name(model, &toolbox_iter, "expandedfolder", &expanded_folder_iter)) {
        fail(test, "'expandedfolder' row missing before refresh");
        goto done;
    }
    GtkTreePath *expanded_path_before = gtk_tree_model_get_path(model, &expanded_folder_iter);
    gboolean expanded_before = gtk_tree_view_row_expanded(GTK_TREE_VIEW(tree_view), expanded_path_before);
    gtk_tree_path_free(expanded_path_before);
    if (!expanded_before) {
        fail(test, "'expandedfolder' should already be expanded after creating a child inside it");
        goto done;
    }

    GtkWidget *refresh_button = find_by_data_key(GTK_WIDGET(window), "toolbox-explorer-refresh-button");
    if (!refresh_button) {
        fail(test, "Refresh toolbar button not found");
        goto done;
    }
    gtk_button_clicked(GTK_BUTTON(refresh_button));

    if (!find_child_by_name(model, &toolbox_iter, "expandedfolder", &expanded_folder_iter)) {
        fail(test, "'expandedfolder' row missing after refresh");
        goto done;
    }
    GtkTreePath *expanded_path_after = gtk_tree_model_get_path(model, &expanded_folder_iter);
    gboolean expanded_after = gtk_tree_view_row_expanded(GTK_TREE_VIEW(tree_view), expanded_path_after);
    gtk_tree_path_free(expanded_path_after);
    if (!expanded_after) {
        fail(test, "refresh should have preserved 'expandedfolder's expanded state");
        goto done;
    }
    GtkTreeIter child_iter;
    if (!find_child_by_name(model, &expanded_folder_iter, "child.txt", &child_iter)) {
        fail(test, "'child.txt' should still be visible under expandedfolder after refresh");
        goto done;
    }

    /* The TOOLBOX root never offers Rename/Delete. */
    menu = open_menu_for_row(tree_view, model, &toolbox_iter);
    if (!menu) {
        fail(test, "TOOLBOX root context menu did not appear");
        goto done;
    }
    if (find_menu_item(menu, "Rename") || find_menu_item(menu, "Delete")) {
        fail(test, "TOOLBOX root's context menu must not offer Rename/Delete");
        goto done;
    }
    if (!find_menu_item(menu, "New File") || !find_menu_item(menu, "New Folder") ||
        !find_menu_item(menu, "Refresh") || !find_menu_item(menu, "Properties")) {
        fail(test, "TOOLBOX root's context menu should offer New File/New Folder/Refresh/Properties");
        goto done;
    }
    /* Every earlier menu in this test was dismissed by activating one of
     * its items, which releases GTK's popup grab as a side effect of
     * gtk_menu_shell_deactivate() running internally - this one is only
     * ever inspected, never activated, so it must be popped down
     * explicitly or its still-active grab blocks the window from
     * actually closing below. */
    gtk_menu_popdown(GTK_MENU(menu));

    test->done = TRUE;

done:
    gtk_window_close(window);
    return G_SOURCE_REMOVE;
}

/* Every GTK smoke test binary lives in the same build/tests/ directory,
 * so workspace_root_init()'s exe-relative resolution finds the *same*
 * physical files/ directory for all of them - ctest runs them back to
 * back with no cleanup in between. Clearing any pre-existing top-level
 * entries here (left over from an earlier test's own fixtures) keeps
 * this test's TOOLBOX-child assertions correct regardless of run order. */
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
        fprintf(stderr, "explorer_operations_smoke: app_create failed\n");
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
        fprintf(stderr, "explorer_operations_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "explorer_operations_smoke: test did not complete\n");
        return 1;
    }

    g_print("explorer_operations_smoke: toolbar create, context-menu create/rename/delete (immediate and "
            "confirmed), refresh-preserves-expansion, and the TOOLBOX root's restricted menu all verified\n");
    return 0;
}
