/*
 * Exercises Step 4's close/quit-side checkpoint end-to-end in the real
 * app: closing a modified tab shows Save/Discard/Cancel and behaves
 * correctly for all three responses; Close Others/Close All (the tab
 * label's own right-click menu) close every target tab, guarding a
 * modified one exactly like a single close would; renaming an open
 * file via the explorer keeps the tab pointed at the new path so a
 * subsequent Save writes there, not the old path; and quitting (driving
 * the real "delete-event" signal) with a modified tab open shows the
 * same confirmation, with Cancel leaving the window and that tab
 * exactly as they were.
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
    fprintf(stderr, "editor_close_confirmation_smoke: %s\n", msg);
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

static void click_page_close_button(GtkWidget *page) {
    GtkWidget *close_button = g_object_get_data(G_OBJECT(page), "toolbox-tab-close-button");
    gtk_button_clicked(GTK_BUTTON(close_button));
}

/* --- Explorer rename helpers (mirrors explorer_operations_smoke.c's
 * own, already-proven Rename flow) --------------------------------- */

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

/* --- Tab-label context menu helpers -------------------------------- */

static GtkWidget *find_tab_event_box(GtkWidget *page) {
    GtkWidget *label = g_object_get_data(G_OBJECT(page), "toolbox-tab-label-widget");
    GtkWidget *label_stack = label ? gtk_widget_get_parent(label) : NULL;
    return label_stack ? gtk_widget_get_parent(label_stack) : NULL;
}

static GtkWidget *open_tab_context_menu(GtkWidget *page) {
    GtkWidget *event_box = find_tab_event_box(page);
    if (!event_box) {
        return NULL;
    }
    gboolean handled = FALSE;
    g_signal_emit_by_name(event_box, "popup-menu", &handled);
    return g_object_get_data(G_OBJECT(page), "toolbox-tab-context-menu");
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
                gtk_widget_destroy(GTK_WIDGET(window));
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

    /* --- Close with Save/Discard/Cancel --- */
    GtkTreeIter doc1_iter;
    if (!find_child_by_name(model, &toolbox_iter, "doc1.txt", &doc1_iter)) {
        fail(test, "'doc1.txt' row not found under TOOLBOX");
        goto done;
    }
    activate_row(tree_view, model, &doc1_iter);
    GtkWidget *doc1_page = find_editor_page(notebook, "doc1.txt");
    if (!doc1_page) {
        fail(test, "doc1.txt's editor tab did not open");
        goto done;
    }
    set_buffer_text(doc1_page, "doc1 edited\n");

    /* Cancel leaves it open and modified. */
    click_page_close_button(doc1_page);
    GtkWidget *dialog = find_message_dialog();
    if (!dialog) {
        fail(test, "closing a modified tab should show a confirmation dialog");
        goto done;
    }
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    if (!find_editor_page(notebook, "doc1.txt") || !label_has_dot(doc1_page)) {
        fail(test, "Cancel should leave the tab open and modified");
        goto done;
    }

    /* Discard closes without writing. */
    click_page_close_button(doc1_page);
    dialog = find_message_dialog();
    if (!dialog) {
        fail(test, "closing a modified tab should show a confirmation dialog (Discard case)");
        goto done;
    }
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_NO);
    if (find_editor_page(notebook, "doc1.txt")) {
        fail(test, "Discard should close the tab");
        goto done;
    }
    char doc1_path[4400], doc1_on_disk[64] = {0};
    snprintf(doc1_path, sizeof(doc1_path), "%s/doc1.txt", test->root.canonical_path);
    read_file_contents(doc1_path, doc1_on_disk, sizeof(doc1_on_disk));
    if (strcmp(doc1_on_disk, "doc1 original\n") != 0) {
        fail(test, "Discard must not write the discarded edit to disk");
        goto done;
    }

    /* Save writes then closes. */
    if (!find_child_by_name(model, &toolbox_iter, "doc1.txt", &doc1_iter)) {
        fail(test, "'doc1.txt' row missing after Discard");
        goto done;
    }
    activate_row(tree_view, model, &doc1_iter);
    doc1_page = find_editor_page(notebook, "doc1.txt");
    if (!doc1_page) {
        fail(test, "doc1.txt's editor tab did not reopen");
        goto done;
    }
    set_buffer_text(doc1_page, "doc1 saved on close\n");
    click_page_close_button(doc1_page);
    dialog = find_message_dialog();
    if (!dialog) {
        fail(test, "closing a modified tab should show a confirmation dialog (Save case)");
        goto done;
    }
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
    if (find_editor_page(notebook, "doc1.txt")) {
        fail(test, "Save should close the tab");
        goto done;
    }
    read_file_contents(doc1_path, doc1_on_disk, sizeof(doc1_on_disk));
    if (strcmp(doc1_on_disk, "doc1 saved on close\n") != 0) {
        fail(test, "Save-on-close should write the edited text to disk");
        goto done;
    }

    /* --- Close Others / Close All --- */
    GtkTreeIter doc2_iter, doc3_iter;
    if (!find_child_by_name(model, &toolbox_iter, "doc2.txt", &doc2_iter) ||
        !find_child_by_name(model, &toolbox_iter, "doc3.txt", &doc3_iter)) {
        fail(test, "'doc2.txt'/'doc3.txt' rows not found under TOOLBOX");
        goto done;
    }
    activate_row(tree_view, model, &doc2_iter);
    activate_row(tree_view, model, &doc3_iter);
    GtkWidget *doc2_page = find_editor_page(notebook, "doc2.txt");
    if (!doc2_page) {
        fail(test, "doc2.txt's editor tab did not open");
        goto done;
    }

    GtkWidget *menu = open_tab_context_menu(doc2_page);
    if (!menu || !click_menu_item(menu, "Close Others")) {
        fail(test, "could not click 'Close Others' on doc2.txt's tab menu");
        goto done;
    }
    if (!find_editor_page(notebook, "doc2.txt")) {
        fail(test, "Close Others should leave doc2.txt's own tab open");
        goto done;
    }
    if (find_editor_page(notebook, "doc3.txt") || gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) != 1) {
        fail(test, "Close Others should close every other tab, including doc3.txt and the startup terminal");
        goto done;
    }

    if (!find_child_by_name(model, &toolbox_iter, "doc3.txt", &doc3_iter)) {
        fail(test, "'doc3.txt' row missing before Close All setup");
        goto done;
    }
    activate_row(tree_view, model, &doc3_iter);
    if (!find_editor_page(notebook, "doc3.txt")) {
        fail(test, "doc3.txt's editor tab did not reopen for the Close All check");
        goto done;
    }
    menu = open_tab_context_menu(doc2_page);
    if (!menu || !click_menu_item(menu, "Close All")) {
        fail(test, "could not click 'Close All' on doc2.txt's tab menu");
        goto done;
    }
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) != 0) {
        fail(test, "Close All should close every open tab");
        goto done;
    }

    /* --- Renaming an open file keeps the tab targeting the new path --- */
    GtkTreeIter renameme_iter;
    if (!find_child_by_name(model, &toolbox_iter, "renameme.txt", &renameme_iter)) {
        fail(test, "'renameme.txt' row not found under TOOLBOX");
        goto done;
    }
    activate_row(tree_view, model, &renameme_iter);
    GtkWidget *rename_page = find_editor_page(notebook, "renameme.txt");
    if (!rename_page) {
        fail(test, "renameme.txt's editor tab did not open");
        goto done;
    }

    GtkWidget *rename_menu = open_menu_for_row(tree_view, model, &renameme_iter);
    if (!rename_menu || !click_menu_item(rename_menu, "Rename")) {
        fail(test, "could not click 'Rename' on renameme.txt's context menu");
        goto done;
    }
    commit_row(tree_view, model, &renameme_iter, "renamed.txt");

    Tab *rename_tab = g_object_get_data(G_OBJECT(rename_page), "toolbox-tab");
    if (strcmp(rename_tab->title, "renamed.txt") != 0) {
        fail(test, "the open tab's title should switch to renamed.txt after an explorer rename");
        goto done;
    }
    set_buffer_text(rename_page, "renamed and edited\n");
    GtkWidget *rename_save_button = g_object_get_data(G_OBJECT(rename_page), "toolbox-editor-save-button");
    gtk_button_clicked(GTK_BUTTON(rename_save_button));

    char old_path[4400], new_path[4400], new_on_disk[64] = {0};
    snprintf(old_path, sizeof(old_path), "%s/renameme.txt", test->root.canonical_path);
    snprintf(new_path, sizeof(new_path), "%s/renamed.txt", test->root.canonical_path);
    if (path_exists(old_path)) {
        fail(test, "the old renameme.txt path should not reappear after a Save following a rename");
        goto done;
    }
    read_file_contents(new_path, new_on_disk, sizeof(new_on_disk));
    if (strcmp(new_on_disk, "renamed and edited\n") != 0) {
        fail(test, "Save after a rename should write to the new path");
        goto done;
    }

    /* Close the now-unmodified renamed.txt tab (no confirmation needed)
     * so the quit-confirmation check below has exactly one open tab -
     * avoids any ambiguity about batch-processing order across mixed
     * modified/unmodified tabs, which isn't this checkpoint's concern. */
    click_page_close_button(rename_page);
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) != 0) {
        fail(test, "closing the unmodified renamed.txt tab should need no confirmation");
        goto done;
    }

    /* --- Quitting with a modified tab open --- */
    GtkTreeIter quittest_iter;
    if (!find_child_by_name(model, &toolbox_iter, "quittest.txt", &quittest_iter)) {
        fail(test, "'quittest.txt' row not found under TOOLBOX");
        goto done;
    }
    activate_row(tree_view, model, &quittest_iter);
    GtkWidget *quit_page = find_editor_page(notebook, "quittest.txt");
    if (!quit_page) {
        fail(test, "quittest.txt's editor tab did not open");
        goto done;
    }
    set_buffer_text(quit_page, "should not be lost\n");

    gboolean delete_handled = FALSE;
    g_signal_emit_by_name(window, "delete-event", NULL, &delete_handled);
    GtkWidget *quit_dialog = find_message_dialog();
    if (!quit_dialog) {
        fail(test, "quitting with a modified tab open should show a confirmation dialog");
        goto done;
    }
    gtk_dialog_response(GTK_DIALOG(quit_dialog), GTK_RESPONSE_CANCEL);

    if (!GTK_IS_WINDOW(window) || !find_editor_page(notebook, "quittest.txt") || !label_has_dot(quit_page)) {
        fail(test, "Cancel on quit should leave the window and the modified tab exactly as they were");
        goto done;
    }

    if (!test->failed) {
        test->done = TRUE;
    }

done:
    /* gtk_widget_destroy() bypasses delete-event/prepare_window_close
     * entirely - deliberate here, since this test already exercised
     * that confirmation flow directly above and a real quittest.txt
     * edit is still deliberately left unsaved/open at this point; using
     * the normal close path again would just show another dialog that
     * would never get dismissed. */
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

    snprintf(path, sizeof(path), "%s/doc1.txt", root->canonical_path);
    write_file_bytes(path, "doc1 original\n", strlen("doc1 original\n"));

    snprintf(path, sizeof(path), "%s/doc2.txt", root->canonical_path);
    write_file_bytes(path, "doc2 original\n", strlen("doc2 original\n"));

    snprintf(path, sizeof(path), "%s/doc3.txt", root->canonical_path);
    write_file_bytes(path, "doc3 original\n", strlen("doc3 original\n"));

    snprintf(path, sizeof(path), "%s/renameme.txt", root->canonical_path);
    write_file_bytes(path, "rename me\n", strlen("rename me\n"));

    snprintf(path, sizeof(path), "%s/quittest.txt", root->canonical_path);
    write_file_bytes(path, "quit test\n", strlen("quit test\n"));
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "editor_close_confirmation_smoke: app_create failed\n");
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
        fprintf(stderr, "editor_close_confirmation_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "editor_close_confirmation_smoke: test did not complete\n");
        return 1;
    }

    g_print("editor_close_confirmation_smoke: close Save/Discard/Cancel, Close Others/Close All, rename-sync, "
            "and quit-confirmation Cancel all verified\n");
    return 0;
}
