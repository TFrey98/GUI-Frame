/*
 * Exercises Step 7's checkpoint end-to-end in the real app: Copy+Paste
 * (duplicate with correct content, original untouched, auto-renamed on
 * a same-folder collision); Cut+Paste (moved, clipboard consumed - the
 * toolbar Paste button goes insensitive again after one paste - and an
 * open editor tab for the cut file keeps tracking it at its new path);
 * the toolbar Paste button starting insensitive and enabling after
 * Copy/Cut; Paste offered on a directory row but never a plain file
 * row; Cut/Copy never offered on the permanent roots; a cross-root case
 * (Toolkit -> TOOLBOX) proving file_copy()'s cross-root path works end
 * to end through the real UI; and pasting a folder into its own
 * subfolder rejected with an error rather than corrupting anything.
 */
#include <dirent.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/app.h"
#include "core/tab.h"
#include "files/editor_document.h"
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
    fprintf(stderr, "explorer_clipboard_smoke: %s\n", msg);
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

static gboolean path_exists(const char *absolute_path) {
    return access(absolute_path, F_OK) == 0;
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

static GtkWidget *find_message_dialog(void) {
    GtkWidget *result = NULL;
    GList *toplevels = gtk_window_list_toplevels();
    for (GList *l = toplevels; l; l = l->next) {
        if (GTK_IS_MESSAGE_DIALOG(l->data)) {
            result = GTK_WIDGET(l->data);
            break;
        }
    }
    g_list_free(toplevels);
    return result;
}

static gboolean drive(gpointer user_data) {
    TestState *test = user_data;

    GtkWindow *window = main_window();
    GtkWidget *tree_view = window ? find_by_data_key(GTK_WIDGET(window), "workbench-explorer-tree") : NULL;
    GtkWidget *notebook = NULL;
    if (window) {
        GPtrArray *notebooks = g_ptr_array_new();
        collect_by_type(GTK_WIDGET(window), notebooks, GTK_TYPE_NOTEBOOK);
        if (notebooks->len > 0) {
            notebook = GTK_WIDGET(g_ptr_array_index(notebooks, 0));
        }
        g_ptr_array_free(notebooks, TRUE);
    }

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

    GtkWidget *paste_button = find_by_data_key(GTK_WIDGET(window), "workbench-explorer-paste-button");
    if (!paste_button) {
        fail(test, "Paste toolbar button not found");
        goto done;
    }
    if (gtk_widget_get_sensitive(paste_button)) {
        fail(test, "the Paste toolbar button should start insensitive - nothing has been Cut/Copied yet");
        goto done;
    }

    /* Cut/Copy are never offered on the permanent TOOLBOX root; Paste is
     * (it's a directory). */
    GtkWidget *menu = open_menu_for_row(tree_view, model, &workbench_iter);
    if (!menu) {
        fail(test, "TOOLBOX root context menu did not appear");
        goto done;
    }
    if (find_menu_item(menu, "Cut") || find_menu_item(menu, "Copy")) {
        fail(test, "the TOOLBOX root's context menu must not offer Cut/Copy");
        goto done;
    }
    if (!find_menu_item(menu, "Paste")) {
        fail(test, "the TOOLBOX root's context menu should offer Paste");
        goto done;
    }
    gtk_menu_popdown(GTK_MENU(menu));

    /* A plain file's context menu offers Cut/Copy, never Paste. */
    GtkTreeIter copysrc_iter;
    if (!find_child_by_name(model, &workbench_iter, "copysrc.txt", &copysrc_iter)) {
        fail(test, "'copysrc.txt' row not found under TOOLBOX");
        goto done;
    }
    menu = open_menu_for_row(tree_view, model, &copysrc_iter);
    if (!menu) {
        fail(test, "copysrc.txt's context menu did not appear");
        goto done;
    }
    if (find_menu_item(menu, "Paste")) {
        fail(test, "a plain file's context menu must not offer Paste");
        goto done;
    }
    if (!click_menu_item(menu, "Copy")) {
        fail(test, "could not click 'Copy' on copysrc.txt");
        goto done;
    }

    if (!gtk_widget_get_sensitive(paste_button)) {
        fail(test, "the Paste toolbar button should become sensitive right after Copy");
        goto done;
    }

    /* Toolbar Paste with copysrc.txt still selected (a file) targets its
     * parent - TOOLBOX - which already has a 'copysrc.txt', so this
     * exercises the auto-rename-on-collision path. */
    gtk_button_clicked(GTK_BUTTON(paste_button));

    char copysrc_path[4400], copysrc_copy_path[4400];
    snprintf(copysrc_path, sizeof(copysrc_path), "%s/copysrc.txt", test->files_root.canonical_path);
    snprintf(copysrc_copy_path, sizeof(copysrc_copy_path), "%s/copysrc.txt (copy)", test->files_root.canonical_path);
    if (!path_exists(copysrc_path)) {
        fail(test, "Copy+Paste must leave the original 'copysrc.txt' untouched");
        goto done;
    }
    if (!path_exists(copysrc_copy_path)) {
        fail(test, "pasting a copy into the same folder should auto-rename to 'copysrc.txt (copy)'");
        goto done;
    }
    char copy_contents[64] = {0};
    if (read_file_contents(copysrc_copy_path, copy_contents, sizeof(copy_contents)) != 0 ||
        strcmp(copy_contents, "copy me\n") != 0) {
        fail(test, "the pasted copy should have the original's content");
        goto done;
    }
    if (!gtk_widget_get_sensitive(paste_button)) {
        fail(test, "Copy mode should stay pasteable more than once - the clipboard isn't consumed");
        goto done;
    }

    /* Cut+Paste: open cutsrc.txt as an editor tab first, cut it, paste
     * it into destfolder, and confirm the open tab keeps tracking it at
     * its new path. */
    GtkTreeIter cutsrc_iter;
    if (!find_child_by_name(model, &workbench_iter, "cutsrc.txt", &cutsrc_iter)) {
        fail(test, "'cutsrc.txt' row not found under TOOLBOX");
        goto done;
    }
    GtkTreePath *cutsrc_path_obj = gtk_tree_model_get_path(model, &cutsrc_iter);
    gtk_tree_view_row_activated(GTK_TREE_VIEW(tree_view), cutsrc_path_obj,
                                 gtk_tree_view_get_column(GTK_TREE_VIEW(tree_view), 0));
    gtk_tree_path_free(cutsrc_path_obj);
    GtkWidget *cutsrc_page = find_editor_page(notebook, "cutsrc.txt");
    if (!cutsrc_page) {
        fail(test, "'cutsrc.txt' did not open as an editor tab");
        goto done;
    }

    menu = open_menu_for_row(tree_view, model, &cutsrc_iter);
    if (!menu || !click_menu_item(menu, "Cut")) {
        fail(test, "could not click 'Cut' on cutsrc.txt");
        goto done;
    }

    GtkTreeIter destfolder_iter;
    if (!find_child_by_name(model, &workbench_iter, "destfolder", &destfolder_iter)) {
        fail(test, "'destfolder' row not found under TOOLBOX");
        goto done;
    }
    menu = open_menu_for_row(tree_view, model, &destfolder_iter);
    if (!menu || !find_menu_item(menu, "Paste")) {
        fail(test, "destfolder's context menu should offer Paste after a Cut");
        goto done;
    }
    if (!click_menu_item(menu, "Paste")) {
        fail(test, "could not click 'Paste' on destfolder");
        goto done;
    }

    char cutsrc_old_path[4400], cutsrc_new_path[4400];
    snprintf(cutsrc_old_path, sizeof(cutsrc_old_path), "%s/cutsrc.txt", test->files_root.canonical_path);
    snprintf(cutsrc_new_path, sizeof(cutsrc_new_path), "%s/destfolder/cutsrc.txt", test->files_root.canonical_path);
    if (path_exists(cutsrc_old_path)) {
        fail(test, "Cut+Paste should remove 'cutsrc.txt' from its old location");
        goto done;
    }
    if (!path_exists(cutsrc_new_path)) {
        fail(test, "Cut+Paste should create 'destfolder/cutsrc.txt'");
        goto done;
    }
    if (gtk_widget_get_sensitive(paste_button)) {
        fail(test, "a Cut's clipboard should be consumed after one successful paste");
        goto done;
    }

    Tab *cutsrc_tab = g_object_get_data(G_OBJECT(cutsrc_page), "workbench-tab");
    EditorDocument *cutsrc_doc = cutsrc_tab->backend_data;
    if (strcmp(cutsrc_doc->relative_path, "destfolder/cutsrc.txt") != 0) {
        fail(test, "the open editor tab for a cut-and-pasted file should keep tracking its new path");
        goto done;
    }

    /* Cross-root: Copy a Toolkit file, Paste it into TOOLBOX's
     * destfolder - proves file_copy()'s cross-root path end to end. */
    GtkTreeIter toolkitfile_iter;
    if (!find_child_by_name(model, &toolkit_iter, "toolkitfile.txt", &toolkitfile_iter)) {
        fail(test, "'toolkitfile.txt' row not found under Toolkit");
        goto done;
    }
    menu = open_menu_for_row(tree_view, model, &toolkitfile_iter);
    if (!menu || !click_menu_item(menu, "Copy")) {
        fail(test, "could not click 'Copy' on toolkitfile.txt");
        goto done;
    }

    if (!find_child_by_name(model, &workbench_iter, "destfolder", &destfolder_iter)) {
        fail(test, "'destfolder' row missing before the cross-root paste");
        goto done;
    }
    menu = open_menu_for_row(tree_view, model, &destfolder_iter);
    if (!menu || !click_menu_item(menu, "Paste")) {
        fail(test, "could not click 'Paste' on destfolder for the cross-root case");
        goto done;
    }

    char toolkitfile_src_path[4400], toolkitfile_dest_path[4400];
    snprintf(toolkitfile_src_path, sizeof(toolkitfile_src_path), "%s/toolkitfile.txt",
              test->toolkit_root.canonical_path);
    snprintf(toolkitfile_dest_path, sizeof(toolkitfile_dest_path), "%s/destfolder/toolkitfile.txt",
              test->files_root.canonical_path);
    if (!path_exists(toolkitfile_src_path)) {
        fail(test, "cross-root Copy+Paste must leave the original Toolkit file untouched");
        goto done;
    }
    if (!path_exists(toolkitfile_dest_path)) {
        fail(test, "cross-root Copy+Paste should create 'destfolder/toolkitfile.txt' under TOOLBOX");
        goto done;
    }
    char toolkitfile_contents[64] = {0};
    if (read_file_contents(toolkitfile_dest_path, toolkitfile_contents, sizeof(toolkitfile_contents)) != 0 ||
        strcmp(toolkitfile_contents, "toolkit content\n") != 0) {
        fail(test, "the cross-root pasted copy should have the Toolkit original's content");
        goto done;
    }

    /* Pasting a folder into its own subfolder is rejected with an error,
     * not a hang or a corrupted recursive copy. */
    GtkTreeIter selffolder_iter;
    if (!find_child_by_name(model, &workbench_iter, "selffolder", &selffolder_iter)) {
        fail(test, "'selffolder' row not found under TOOLBOX");
        goto done;
    }
    GtkTreePath *selffolder_path_obj = gtk_tree_model_get_path(model, &selffolder_iter);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_view), selffolder_path_obj, FALSE);
    gtk_tree_path_free(selffolder_path_obj);

    menu = open_menu_for_row(tree_view, model, &selffolder_iter);
    if (!menu || !click_menu_item(menu, "Cut")) {
        fail(test, "could not click 'Cut' on selffolder");
        goto done;
    }

    GtkTreeIter inner_iter;
    if (!find_child_by_name(model, &selffolder_iter, "inner", &inner_iter)) {
        fail(test, "'selffolder/inner' row not found");
        goto done;
    }
    menu = open_menu_for_row(tree_view, model, &inner_iter);
    if (!menu || !click_menu_item(menu, "Paste")) {
        fail(test, "could not click 'Paste' on selffolder/inner");
        goto done;
    }

    GtkWidget *nesting_error_dialog = find_message_dialog();
    if (!nesting_error_dialog) {
        fail(test, "pasting a folder into its own subfolder should show an error dialog");
        goto done;
    }
    gtk_dialog_response(GTK_DIALOG(nesting_error_dialog), GTK_RESPONSE_OK);

    char selffolder_path[4400], nested_copy_path[4400];
    snprintf(selffolder_path, sizeof(selffolder_path), "%s/selffolder", test->files_root.canonical_path);
    snprintf(nested_copy_path, sizeof(nested_copy_path), "%s/selffolder/inner/selffolder",
              test->files_root.canonical_path);
    if (!path_exists(selffolder_path)) {
        fail(test, "the rejected self-nesting move must leave 'selffolder' at its original location");
        goto done;
    }
    if (path_exists(nested_copy_path)) {
        fail(test, "the rejected self-nesting paste must not have created a nested copy");
        goto done;
    }

    test->done = TRUE;

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

    snprintf(path, sizeof(path), "%s/copysrc.txt", files_root->canonical_path);
    write_file_bytes(path, "copy me\n", strlen("copy me\n"));

    snprintf(path, sizeof(path), "%s/cutsrc.txt", files_root->canonical_path);
    write_file_bytes(path, "cut me\n", strlen("cut me\n"));

    snprintf(path, sizeof(path), "%s/destfolder", files_root->canonical_path);
    mkdir(path, 0755);

    snprintf(path, sizeof(path), "%s/selffolder", files_root->canonical_path);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/selffolder/inner", files_root->canonical_path);
    mkdir(path, 0755);

    snprintf(path, sizeof(path), "%s/toolkitfile.txt", toolkit_root->canonical_path);
    write_file_bytes(path, "toolkit content\n", strlen("toolkit content\n"));
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "explorer_clipboard_smoke: app_create failed\n");
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
        fprintf(stderr, "explorer_clipboard_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "explorer_clipboard_smoke: test did not complete\n");
        return 1;
    }

    g_print("explorer_clipboard_smoke: Copy+Paste (with auto-rename collision), Cut+Paste (with open-tab tracking "
            "and clipboard consumption), Paste toolbar sensitivity, Cut/Copy/Paste menu gating, a cross-root Copy, "
            "and the self-nesting paste guard all verified\n");
    return 0;
}
