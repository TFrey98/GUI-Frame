/*
 * Exercises the manifest-driven bottom-panel tab end-to-end in the real
 * app: a fixture tool (a script + a sibling *.manifest.json under
 * toolkit/) launched via the real "Run in Terminal" context-menu action
 * gets its own bottom-panel tab, rows appear as the script appends
 * JSON-Lines to its declared data_file (driven by the real FileWatcher,
 * not a manual refresh), and the tab is marked "(stopped)" once the
 * script's process actually exits. The built-in Objects tab (page 0)
 * must remain untouched throughout.
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
#define STEP_TIMEOUT_MS 8000

typedef enum Step {
    STEP_WAIT_UI,
    STEP_RUN_TOOL,
    STEP_WAIT_TAB_APPEARS,
    STEP_WAIT_ROWS,
    STEP_WAIT_STOPPED,
} Step;

typedef struct TestState {
    Step step;
    int step_elapsed_ms;
    gboolean failed;
    gboolean done;
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
    fprintf(stderr, "tool_panel_smoke: %s\n", msg);
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

/* Distinguishes the bottom-panel notebook from the main workspace tabs
 * notebook (both are plain GtkNotebooks now) by its permanent, known
 * page-0 label - see build_bottom_panel in ui_gtk_object_list.c. */
static GtkWidget *find_bottom_panel_notebook(GPtrArray *notebooks) {
    for (guint i = 0; i < notebooks->len; i++) {
        GtkWidget *candidate = GTK_WIDGET(g_ptr_array_index(notebooks, i));
        if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(candidate)) < 1) {
            continue;
        }
        GtkWidget *first_page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(candidate), 0);
        GtkWidget *label = gtk_notebook_get_tab_label(GTK_NOTEBOOK(candidate), first_page);
        if (GTK_IS_LABEL(label) && strcmp(gtk_label_get_text(GTK_LABEL(label)), "Objects") == 0) {
            return candidate;
        }
    }
    return NULL;
}

static const char *tab_label_text(GtkNotebook *notebook, int page_index) {
    GtkWidget *page = gtk_notebook_get_nth_page(notebook, page_index);
    GtkWidget *label_box = gtk_notebook_get_tab_label(notebook, page);
    GPtrArray *labels = g_ptr_array_new();
    collect_by_type(label_box, labels, GTK_TYPE_LABEL);
    const char *text = labels->len > 0 ? gtk_label_get_text(GTK_LABEL(g_ptr_array_index(labels, 0))) : NULL;
    g_ptr_array_free(labels, TRUE);
    return text;
}

/* Reads every row of the tool panel tab's 2-column (Host, Port) store
 * into a "host:port" line per row, joined by ';' - just enough to
 * assert both fixture rows landed with the right values. */
static void tool_panel_rows_summary(GtkNotebook *notebook, int page_index, char *out, size_t out_size) {
    out[0] = '\0';
    GtkWidget *page = gtk_notebook_get_nth_page(notebook, page_index);
    GPtrArray *tree_views = g_ptr_array_new();
    collect_by_type(page, tree_views, GTK_TYPE_TREE_VIEW);
    if (tree_views->len == 0) {
        g_ptr_array_free(tree_views, TRUE);
        return;
    }
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(g_ptr_array_index(tree_views, 0)));
    g_ptr_array_free(tree_views, TRUE);

    GtkTreeIter iter;
    gboolean has_row = gtk_tree_model_get_iter_first(model, &iter);
    while (has_row) {
        gchar *host = NULL, *port = NULL;
        gtk_tree_model_get(model, &iter, 0, &host, 1, &port, -1);
        char line[256];
        snprintf(line, sizeof(line), "%s:%s;", host ? host : "", port ? port : "");
        g_strlcat(out, line, out_size);
        g_free(host);
        g_free(port);
        has_row = gtk_tree_model_iter_next(model, &iter);
    }
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

    for (;;) {
        GtkWindow *window = main_window();
        if (!window) {
            return G_SOURCE_REMOVE;
        }
        GtkWidget *tree_view = find_by_data_key(GTK_WIDGET(window), "workbench-explorer-tree");
        GPtrArray *notebooks = g_ptr_array_new();
        collect_by_type(GTK_WIDGET(window), notebooks, GTK_TYPE_NOTEBOOK);
        GtkWidget *bottom_panel = find_bottom_panel_notebook(notebooks);
        g_ptr_array_free(notebooks, TRUE);

        switch (test->step) {
            case STEP_WAIT_UI: {
                if (!tree_view || !bottom_panel) {
                    break;
                }
                GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));
                GtkTreeIter workbench_iter, toolkit_iter;
                if (!gtk_tree_model_get_iter_first(model, &workbench_iter) ||
                    !row_name_is(model, &workbench_iter, "TOOLBOX")) {
                    break;
                }
                toolkit_iter = workbench_iter;
                if (!gtk_tree_model_iter_next(model, &toolkit_iter) || !row_name_is(model, &toolkit_iter, "Toolkit")) {
                    break;
                }
                if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(bottom_panel)) != 1) {
                    fail(test, "the bottom panel should start with exactly one page (Objects)");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_RUN_TOOL;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_RUN_TOOL: {
                GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_view));
                GtkTreeIter workbench_iter, toolkit_iter, script_iter;
                gtk_tree_model_get_iter_first(model, &workbench_iter);
                toolkit_iter = workbench_iter;
                gtk_tree_model_iter_next(model, &toolkit_iter);
                if (!find_child_by_name(model, &toolkit_iter, "scan.sh", &script_iter)) {
                    fail(test, "'scan.sh' row not found under Toolkit");
                    return G_SOURCE_REMOVE;
                }
                GtkWidget *menu = open_menu_for_row(tree_view, model, &script_iter);
                if (!menu || !click_menu_item(menu, "Run in Terminal")) {
                    fail(test, "could not click 'Run in Terminal' on scan.sh");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_TAB_APPEARS;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_TAB_APPEARS: {
                if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(bottom_panel)) < 2) {
                    break;
                }
                const char *label = tab_label_text(GTK_NOTEBOOK(bottom_panel), 1);
                if (!label || strcmp(label, "Scan Results") != 0) {
                    fail(test, "the new tool panel tab should be labeled 'Scan Results'");
                    return G_SOURCE_REMOVE;
                }
                test->step = STEP_WAIT_ROWS;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_ROWS: {
                char summary[1024];
                tool_panel_rows_summary(GTK_NOTEBOOK(bottom_panel), 1, summary, sizeof(summary));
                if (!strstr(summary, "10.0.0.1:22;") || !strstr(summary, "10.0.0.2:80;")) {
                    break; /* keep polling - rows land via the real FileWatcher, not synchronously */
                }
                test->step = STEP_WAIT_STOPPED;
                test->step_elapsed_ms = 0;
                continue;
            }

            case STEP_WAIT_STOPPED: {
                const char *label = tab_label_text(GTK_NOTEBOOK(bottom_panel), 1);
                if (!label || strcmp(label, "Scan Results (stopped)") != 0) {
                    break; /* keep polling until the tick notices the process exited */
                }
                if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(bottom_panel)) != 2) {
                    fail(test, "the Objects tab should still be the only other bottom-panel page");
                }
                test->done = TRUE;
                gtk_widget_destroy(GTK_WIDGET(window));
                return G_SOURCE_REMOVE;
            }
        }

        break; /* the current step is a wait that isn't satisfied yet */
    }

    test->step_elapsed_ms += STEP_INTERVAL_MS;
    if (test->step_elapsed_ms >= STEP_TIMEOUT_MS) {
        char msg[64];
        snprintf(msg, sizeof(msg), "timed out waiting on step %d", (int)test->step);
        fail(test, msg);
        GtkWindow *window = main_window();
        if (window) {
            gtk_widget_destroy(GTK_WIDGET(window));
        }
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* Every GTK smoke test binary lives in the same build/tests/ directory,
 * so toolkit_index's exe-relative resolution finds the same physical
 * toolkit/ directory for all of them - clearing any pre-existing
 * top-level entries keeps this test's assertions correct regardless of
 * ctest run order, same precaution toolkit_interaction_smoke.c already
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

static void write_fixtures(const WorkspaceRoot *toolkit_root) {
    char path[4400];

    /* Each redirection is its own open+write+close, so the FileWatcher's
     * IN_CLOSE_WRITE-driven FILE_WATCH_MODIFIED fires twice - once per
     * row - proving the tab updates live rather than only once at the
     * end. No trailing sleep, so the process (and therefore its
     * TerminalSession) exits promptly once both lines are written. */
    snprintf(path, sizeof(path), "%s/scan.sh", toolkit_root->canonical_path);
    const char *script = "#!/bin/sh\n"
                          "DIR=\"$(dirname \"$0\")\"\n"
                          "echo '{\"host\": \"10.0.0.1\", \"port\": \"22\"}' >> \"$DIR/scan.out.jsonl\"\n"
                          "sleep 0.3\n"
                          "echo '{\"host\": \"10.0.0.2\", \"port\": \"80\"}' >> \"$DIR/scan.out.jsonl\"\n";
    write_file_bytes(path, script, strlen(script));
    chmod(path, 0755);

    snprintf(path, sizeof(path), "%s/scan.sh.manifest.json", toolkit_root->canonical_path);
    const char *manifest = "{ \"panel\": { \"title\": \"Scan Results\", \"data_file\": \"scan.out.jsonl\", "
                            "\"columns\": [ {\"key\": \"host\", \"label\": \"Host\"}, "
                            "{\"key\": \"port\", \"label\": \"Port\"} ] } }";
    write_file_bytes(path, manifest, strlen(manifest));
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "tool_panel_smoke: app_create failed\n");
        return 1;
    }
    test.toolkit_root = *app_get_toolkit_workspace_root(app);
    clear_workspace_root(&test.toolkit_root);
    write_fixtures(&test.toolkit_root);

    g_timeout_add(STEP_INTERVAL_MS, drive, &test);

    int status = app_run(app);
    app_destroy(app);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "tool_panel_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "tool_panel_smoke: test did not complete\n");
        return 1;
    }

    g_print("tool_panel_smoke: manifest-driven tab creation, live row updates via the real FileWatcher, and "
            "stopped-state marking on process exit all verified\n");
    return 0;
}
