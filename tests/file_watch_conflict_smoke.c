/*
 * Exercises how open editor tabs react to a file changing externally
 * (from this test process, simulating an external terminal or editor -
 * never through file_operations.c or the explorer UI), end to end in
 * the real running app:
 *   - unedited + externally modified -> silent reload, no dialog.
 *   - edited + externally modified -> the conflict dialog, driven all
 *     three ways: Reload from Disk (buffer becomes the on-disk content),
 *     Keep Editor Version (edits survive, no re-prompt), Compare (a
 *     genuinely separate read-only "(on disk)" tab opens, the edited tab
 *     untouched).
 *   - an open file deleted externally -> the "Deleted from disk" banner.
 *   - saving through the app's own Save button never triggers a
 *     spurious conflict dialog afterward (the mtime-suppression proof).
 * Each scenario uses its own fixture file and runs strictly after the
 * previous one's dialog (if any) has already been responded to and
 * destroyed, so at most one relevant dialog is ever on screen at once.
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
#define NO_DIALOG_WINDOW_MS 800

#define UNEDITED_ORIGINAL "unedited original\n"
#define UNEDITED_EXTERNAL "unedited replaced externally\n"
#define RELOAD_ORIGINAL "reload original\n"
#define RELOAD_EXTERNAL "reload replaced externally\n"
#define EDIT_MARKER " EDITED-IN-APP"
#define KEEP_ORIGINAL "keep original\n"
#define KEEP_EXTERNAL "keep replaced externally\n"
#define COMPARE_ORIGINAL "compare original\n"
#define COMPARE_EXTERNAL "compare replaced externally\n"
#define DELETED_ORIGINAL "deleted original\n"
#define SAVED_ORIGINAL "saved original\n"

typedef enum Stage {
    STAGE_OPEN_ALL_TABS,
    STAGE_TRIGGER_UNEDITED,
    STAGE_WAIT_UNEDITED_RELOAD,
    STAGE_TRIGGER_RELOAD_CASE,
    STAGE_WAIT_RELOAD_DIALOG,
    STAGE_VERIFY_RELOAD,
    STAGE_TRIGGER_KEEP_CASE,
    STAGE_WAIT_KEEP_DIALOG,
    STAGE_VERIFY_KEEP,
    STAGE_TRIGGER_COMPARE_CASE,
    STAGE_WAIT_COMPARE_DIALOG,
    STAGE_VERIFY_COMPARE,
    STAGE_TRIGGER_DELETE_CASE,
    STAGE_WAIT_DELETE_BANNER,
    STAGE_TRIGGER_SAVE_CASE,
    STAGE_WAIT_NO_SPURIOUS_CONFLICT,
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
    fprintf(stderr, "file_watch_conflict_smoke: %s\n", msg);
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

static gchar *editor_buffer_text(GtkWidget *page) {
    GtkWidget *view = g_object_get_data(G_OBJECT(page), "workbench-editor-text-view");
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void append_marker(GtkWidget *page) {
    GtkWidget *view = g_object_get_data(G_OBJECT(page), "workbench-editor-text-view");
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, EDIT_MARKER, -1);
}

static gboolean open_tab(GtkWidget *tree_view, GtkTreeModel *model, GtkTreeIter *workbench_iter, const char *name) {
    GtkTreeIter row;
    if (!find_child_by_name(model, workbench_iter, name, &row)) {
        return FALSE;
    }
    GtkTreePath *path = gtk_tree_model_get_path(model, &row);
    gtk_tree_view_row_activated(GTK_TREE_VIEW(tree_view), path, gtk_tree_view_get_column(GTK_TREE_VIEW(tree_view), 0));
    gtk_tree_path_free(path);
    return TRUE;
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
    GtkWidget *real_notebook = NULL;
    if (window) {
        GPtrArray *notebooks = g_ptr_array_new();
        collect_by_type(GTK_WIDGET(window), notebooks, GTK_TYPE_NOTEBOOK);
        if (notebooks->len > 0) {
            real_notebook = GTK_WIDGET(g_ptr_array_index(notebooks, 0));
        }
        g_ptr_array_free(notebooks, TRUE);
    }

    if (!tree_view || !real_notebook) {
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
    GtkTreeIter workbench_iter;
    if (!gtk_tree_model_get_iter_first(model, &workbench_iter) || !row_name_is(model, &workbench_iter, "TOOLBOX")) {
        fail(test, "expected TOOLBOX as the first top-level row");
        goto done;
    }

    char path_buf[4400];
    test->elapsed_ms += STEP_INTERVAL_MS;

    switch (test->stage) {
        case STAGE_OPEN_ALL_TABS: {
            const char *names[] = {"unedited.txt",     "edited_reload.txt", "edited_keep.txt",
                                    "edited_compare.txt", "deleted_open.txt", "saved_no_conflict.txt"};
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
                if (!open_tab(tree_view, model, &workbench_iter, names[i])) {
                    fail(test, "a fixture row was missing under TOOLBOX at startup");
                    goto done;
                }
            }
            GtkWidget *reload_page = find_editor_page(real_notebook, "edited_reload.txt");
            GtkWidget *keep_page = find_editor_page(real_notebook, "edited_keep.txt");
            GtkWidget *compare_page = find_editor_page(real_notebook, "edited_compare.txt");
            GtkWidget *saved_page = find_editor_page(real_notebook, "saved_no_conflict.txt");
            if (!reload_page || !keep_page || !compare_page || !saved_page) {
                fail(test, "not every fixture opened as a real editor tab");
                goto done;
            }
            append_marker(reload_page);
            append_marker(keep_page);
            append_marker(compare_page);
            append_marker(saved_page);
            test->stage = STAGE_TRIGGER_UNEDITED;
            test->elapsed_ms = 0;
            return G_SOURCE_CONTINUE;
        }

        case STAGE_TRIGGER_UNEDITED:
            snprintf(path_buf, sizeof(path_buf), "%s/unedited.txt", test->root.canonical_path);
            write_file(path_buf, UNEDITED_EXTERNAL);
            test->stage = STAGE_WAIT_UNEDITED_RELOAD;
            test->elapsed_ms = 0;
            return G_SOURCE_CONTINUE;

        case STAGE_WAIT_UNEDITED_RELOAD: {
            GtkWidget *page = find_editor_page(real_notebook, "unedited.txt");
            gchar *text = page ? editor_buffer_text(page) : NULL;
            gboolean reloaded = text && strcmp(text, UNEDITED_EXTERNAL) == 0;
            g_free(text);
            if (reloaded) {
                Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
                EditorDocument *doc = tab->backend_data;
                if (doc->modified) {
                    fail(test, "unedited.txt should silently reload without ever becoming 'modified'");
                    goto done;
                }
                if (find_message_dialog()) {
                    fail(test, "an unedited external modification should never show the conflict dialog");
                    goto done;
                }
                test->stage = STAGE_TRIGGER_RELOAD_CASE;
                test->elapsed_ms = 0;
            } else if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
                fail(test, "unedited.txt never silently reloaded its externally-changed content");
                goto done;
            }
            return G_SOURCE_CONTINUE;
        }

        case STAGE_TRIGGER_RELOAD_CASE:
            snprintf(path_buf, sizeof(path_buf), "%s/edited_reload.txt", test->root.canonical_path);
            write_file(path_buf, RELOAD_EXTERNAL);
            test->stage = STAGE_WAIT_RELOAD_DIALOG;
            test->elapsed_ms = 0;
            return G_SOURCE_CONTINUE;

        case STAGE_WAIT_RELOAD_DIALOG: {
            GtkWidget *dialog = find_message_dialog();
            if (dialog) {
                gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES); /* Reload from Disk */
                test->stage = STAGE_VERIFY_RELOAD;
                test->elapsed_ms = 0;
            } else if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
                fail(test, "editing then externally modifying edited_reload.txt should show the conflict dialog");
                goto done;
            }
            return G_SOURCE_CONTINUE;
        }

        case STAGE_VERIFY_RELOAD: {
            GtkWidget *page = find_editor_page(real_notebook, "edited_reload.txt");
            gchar *text = page ? editor_buffer_text(page) : NULL;
            gboolean matches = text && strcmp(text, RELOAD_EXTERNAL) == 0;
            g_free(text);
            if (matches) {
                Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
                EditorDocument *doc = tab->backend_data;
                if (doc->modified || doc->externally_modified) {
                    fail(test, "'Reload from Disk' should leave the doc unmodified and no longer conflicted");
                    goto done;
                }
                test->stage = STAGE_TRIGGER_KEEP_CASE;
                test->elapsed_ms = 0;
            } else if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
                fail(test, "'Reload from Disk' should replace the buffer with the on-disk content");
                goto done;
            }
            return G_SOURCE_CONTINUE;
        }

        case STAGE_TRIGGER_KEEP_CASE:
            snprintf(path_buf, sizeof(path_buf), "%s/edited_keep.txt", test->root.canonical_path);
            write_file(path_buf, KEEP_EXTERNAL);
            test->stage = STAGE_WAIT_KEEP_DIALOG;
            test->elapsed_ms = 0;
            return G_SOURCE_CONTINUE;

        case STAGE_WAIT_KEEP_DIALOG: {
            GtkWidget *dialog = find_message_dialog();
            if (dialog) {
                gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_NO); /* Keep Editor Version */
                test->stage = STAGE_VERIFY_KEEP;
                test->elapsed_ms = 0;
            } else if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
                fail(test, "editing then externally modifying edited_keep.txt should show the conflict dialog");
                goto done;
            }
            return G_SOURCE_CONTINUE;
        }

        case STAGE_VERIFY_KEEP: {
            GtkWidget *page = find_editor_page(real_notebook, "edited_keep.txt");
            gchar *text = page ? editor_buffer_text(page) : NULL;
            gboolean kept_edit = text && strstr(text, EDIT_MARKER) != NULL && strstr(text, KEEP_ORIGINAL) != NULL;
            g_free(text);
            if (!kept_edit) {
                fail(test, "'Keep Editor Version' should leave the edited buffer completely untouched");
                goto done;
            }
            Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
            EditorDocument *doc = tab->backend_data;
            if (!doc->modified) {
                fail(test, "'Keep Editor Version' should leave the edit's modified state intact");
                goto done;
            }
            if (doc->externally_modified) {
                fail(test, "'Keep Editor Version' should clear externally_modified so it doesn't re-prompt");
                goto done;
            }
            test->stage = STAGE_TRIGGER_COMPARE_CASE;
            test->elapsed_ms = 0;
            return G_SOURCE_CONTINUE;
        }

        case STAGE_TRIGGER_COMPARE_CASE:
            snprintf(path_buf, sizeof(path_buf), "%s/edited_compare.txt", test->root.canonical_path);
            write_file(path_buf, COMPARE_EXTERNAL);
            test->stage = STAGE_WAIT_COMPARE_DIALOG;
            test->elapsed_ms = 0;
            return G_SOURCE_CONTINUE;

        case STAGE_WAIT_COMPARE_DIALOG: {
            GtkWidget *dialog = find_message_dialog();
            if (dialog) {
                gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_APPLY); /* Compare */
                test->stage = STAGE_VERIFY_COMPARE;
                test->elapsed_ms = 0;
            } else if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
                fail(test, "editing then externally modifying edited_compare.txt should show the conflict dialog");
                goto done;
            }
            return G_SOURCE_CONTINUE;
        }

        case STAGE_VERIFY_COMPARE: {
            GtkWidget *edited_page = find_editor_page(real_notebook, "edited_compare.txt");
            GtkWidget *on_disk_page = find_editor_page(real_notebook, "edited_compare.txt (on disk)");
            if (!edited_page || !on_disk_page) {
                if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
                    fail(test, "'Compare' should open a separate 'edited_compare.txt (on disk)' tab");
                    goto done;
                }
                return G_SOURCE_CONTINUE;
            }
            gchar *edited_text = editor_buffer_text(edited_page);
            gchar *on_disk_text = editor_buffer_text(on_disk_page);
            gboolean edited_intact = edited_text && strstr(edited_text, EDIT_MARKER) != NULL;
            gboolean on_disk_correct = on_disk_text && strcmp(on_disk_text, COMPARE_EXTERNAL) == 0;
            g_free(edited_text);
            g_free(on_disk_text);
            if (!edited_intact) {
                fail(test, "'Compare' must never touch the original edited tab's buffer");
                goto done;
            }
            if (!on_disk_correct) {
                fail(test, "the 'Compare' tab should show the real on-disk content");
                goto done;
            }
            GtkWidget *on_disk_view = g_object_get_data(G_OBJECT(on_disk_page), "workbench-editor-text-view");
            if (gtk_text_view_get_editable(GTK_TEXT_VIEW(on_disk_view))) {
                fail(test, "the 'Compare' tab must always be read-only");
                goto done;
            }
            test->stage = STAGE_TRIGGER_DELETE_CASE;
            test->elapsed_ms = 0;
            return G_SOURCE_CONTINUE;
        }

        case STAGE_TRIGGER_DELETE_CASE:
            snprintf(path_buf, sizeof(path_buf), "%s/deleted_open.txt", test->root.canonical_path);
            unlink(path_buf);
            test->stage = STAGE_WAIT_DELETE_BANNER;
            test->elapsed_ms = 0;
            return G_SOURCE_CONTINUE;

        case STAGE_WAIT_DELETE_BANNER: {
            GtkWidget *page = find_editor_page(real_notebook, "deleted_open.txt");
            GtkWidget *banner = page ? g_object_get_data(G_OBJECT(page), "workbench-editor-deleted-banner") : NULL;
            if (banner && gtk_widget_get_visible(banner)) {
                test->stage = STAGE_TRIGGER_SAVE_CASE;
                test->elapsed_ms = 0;
            } else if (test->elapsed_ms >= STEP_TIMEOUT_MS) {
                fail(test, "deleting an open file externally should show the 'Deleted from disk' banner");
                goto done;
            }
            return G_SOURCE_CONTINUE;
        }

        case STAGE_TRIGGER_SAVE_CASE: {
            GtkWidget *page = find_editor_page(real_notebook, "saved_no_conflict.txt");
            GtkWidget *save_button = page ? g_object_get_data(G_OBJECT(page), "workbench-editor-save-button") : NULL;
            if (!save_button) {
                fail(test, "saved_no_conflict.txt's editor tab should have a Save button");
                goto done;
            }
            gtk_button_clicked(GTK_BUTTON(save_button));
            Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
            EditorDocument *doc = tab->backend_data;
            if (doc->modified) {
                fail(test, "clicking Save should clear the modified flag immediately");
                goto done;
            }
            test->stage = STAGE_WAIT_NO_SPURIOUS_CONFLICT;
            test->elapsed_ms = 0;
            return G_SOURCE_CONTINUE;
        }

        case STAGE_WAIT_NO_SPURIOUS_CONFLICT: {
            if (find_message_dialog()) {
                fail(test, "saving through the app's own Save button must never trigger a spurious conflict dialog");
                goto done;
            }
            GtkWidget *page = find_editor_page(real_notebook, "saved_no_conflict.txt");
            Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
            EditorDocument *doc = tab->backend_data;
            if (doc->externally_modified) {
                fail(test, "saving through the app's own Save button should never set externally_modified");
                goto done;
            }
            if (test->elapsed_ms >= NO_DIALOG_WINDOW_MS) {
                test->done = TRUE;
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

static void write_fixtures(const WorkspaceRoot *root) {
    char path[4400];

    snprintf(path, sizeof(path), "%s/unedited.txt", root->canonical_path);
    write_file(path, UNEDITED_ORIGINAL);

    snprintf(path, sizeof(path), "%s/edited_reload.txt", root->canonical_path);
    write_file(path, RELOAD_ORIGINAL);

    snprintf(path, sizeof(path), "%s/edited_keep.txt", root->canonical_path);
    write_file(path, KEEP_ORIGINAL);

    snprintf(path, sizeof(path), "%s/edited_compare.txt", root->canonical_path);
    write_file(path, COMPARE_ORIGINAL);

    snprintf(path, sizeof(path), "%s/deleted_open.txt", root->canonical_path);
    write_file(path, DELETED_ORIGINAL);

    snprintf(path, sizeof(path), "%s/saved_no_conflict.txt", root->canonical_path);
    write_file(path, SAVED_ORIGINAL);
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "file_watch_conflict_smoke: app_create failed\n");
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
        fprintf(stderr, "file_watch_conflict_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "file_watch_conflict_smoke: test did not complete\n");
        return 1;
    }

    g_print("file_watch_conflict_smoke: silent reload, Reload/Keep/Compare conflict responses, deleted-from-disk "
            "banner, and no-spurious-conflict-after-save all verified\n");
    return 0;
}
