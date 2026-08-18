#include "ui_gtk_backend.h"

#include <string.h>

#include "tools/json_parser.h"

/* --- Manifest-driven bottom-panel tabs -----------------------------------
 * Additive to (never routing through) the built-in Objects tab in
 * ui_gtk_object_list.c - see that file's own section comment. Each
 * ToolPanelTab renders its declared columns as plain strings in a flat
 * GtkListStore; unlike the Objects tab's GtkTreeStore, there's no
 * expand state to preserve across a refresh, so refresh_tool_panel_tab
 * below just clears and repopulates the whole store from the data
 * file's current content rather than diffing row by row. */

/* Data files are tool-generated, potentially-growing logs, not
 * hand-authored config like the manifest itself - a much larger cap,
 * but still bounded so a runaway tool can't blow up memory. */
#define TOOL_PANEL_DATA_FILE_MAX_SIZE (8 * 1024 * 1024)

static char *read_data_file(const char *path) {
    gchar *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &contents, &length, NULL)) {
        return NULL; /* doesn't exist yet, or unreadable - treated as "no rows" */
    }
    if (length > TOOL_PANEL_DATA_FILE_MAX_SIZE) {
        g_free(contents);
        return NULL;
    }
    return contents;
}

static char *format_cell(const JsonValue *row, const char *key) {
    const JsonValue *cell = json_object_get(row, key);
    if (!cell) {
        return g_strdup("");
    }
    const char *s;
    double n;
    bool b;
    if (json_as_string(cell, &s)) {
        return g_strdup(s);
    }
    if (json_as_number(cell, &n)) {
        return g_strdup_printf("%g", n);
    }
    if (json_as_bool(cell, &b)) {
        return g_strdup(b ? "true" : "false");
    }
    return g_strdup(""); /* null, or a nested array/object - not a supported cell shape */
}

/* Fully re-reads and re-parses tab_panel's data file as JSON Lines (one
 * JSON object per non-empty line), replacing every row. A line that
 * isn't valid JSON, or that doesn't parse to an object, is skipped
 * rather than aborting the whole refresh - one bad line from a tool
 * mid-write shouldn't blank out everything already captured. */
static void refresh_tool_panel_tab(ToolPanelTab *tab_panel) {
    gtk_list_store_clear(tab_panel->store);

    char *text = read_data_file(tab_panel->data_file_absolute_path);
    if (!text) {
        return;
    }

    char *line = text;
    while (line) {
        char *newline = strchr(line, '\n');
        if (newline) {
            *newline = '\0';
        }
        if (*line != '\0') {
            JsonValue *row = json_parse(line);
            if (row && json_value_type(row) == JSON_OBJECT) {
                GtkTreeIter iter;
                gtk_list_store_append(tab_panel->store, &iter);
                for (int col = 0; col < tab_panel->column_count; col++) {
                    char *cell = format_cell(row, tab_panel->columns[col].key);
                    gtk_list_store_set(tab_panel->store, &iter, col, cell, -1);
                    g_free(cell);
                }
            }
            json_value_free(row);
        }
        line = newline ? newline + 1 : NULL;
    }
    g_free(text);
}

/* Bound to the × on every tool panel tab - removes it from both the
 * notebook and backend->tool_panel_tabs. Unlike a workspace Tab, this
 * never needs a running-tool confirmation dialog: the tool's own
 * terminal tab (with its own close confirmation, if any) is a separate
 * page the user closes independently. */
static void on_tool_panel_close_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWidget *page = GTK_WIDGET(user_data);
    GtkBackend *backend = g_object_get_data(G_OBJECT(page), "workbench-backend");
    ToolPanelTab *tab_panel = g_object_get_data(G_OBJECT(page), "workbench-tool-panel-tab");
    if (!backend || !tab_panel) {
        return;
    }
    gint index = gtk_notebook_page_num(GTK_NOTEBOOK(backend->bottom_panel_notebook), page);
    if (index >= 0) {
        gtk_notebook_remove_page(GTK_NOTEBOOK(backend->bottom_panel_notebook), index);
    }
    g_ptr_array_remove(backend->tool_panel_tabs, tab_panel);
    g_free(tab_panel->title);
    g_free(tab_panel);
}

void open_tool_panel_tab_for_launch(GtkBackend *backend, uint64_t terminal_tab_id,
                                     const ToolPanelManifest *manifest) {
    const WorkspaceRoot *toolkit_root = workbench_get_toolkit_workspace_root(backend->workbench);
    size_t root_len = strlen(toolkit_root->canonical_path);
    /* tool_panel_manifest_load() already guarantees data_file_absolute_path
     * resolves inside toolkit_root - this is just a defensive re-check,
     * never expected to actually fail. */
    if (strncmp(manifest->data_file_absolute_path, toolkit_root->canonical_path, root_len) != 0 ||
        manifest->data_file_absolute_path[root_len] != '/') {
        return;
    }

    ToolPanelTab *tab_panel = g_new0(ToolPanelTab, 1);
    tab_panel->terminal_tab_id = terminal_tab_id;
    tab_panel->title = g_strdup(manifest->title);
    g_strlcpy(tab_panel->data_file_absolute_path, manifest->data_file_absolute_path,
              sizeof(tab_panel->data_file_absolute_path));
    g_strlcpy(tab_panel->data_file_toolkit_relative_path, manifest->data_file_absolute_path + root_len + 1,
              sizeof(tab_panel->data_file_toolkit_relative_path));
    tab_panel->column_count = manifest->column_count;
    memcpy(tab_panel->columns, manifest->columns, sizeof(tab_panel->columns));
    tab_panel->stopped = FALSE;

    GType types[TOOL_PANEL_MANIFEST_MAX_COLUMNS];
    for (int i = 0; i < tab_panel->column_count; i++) {
        types[i] = G_TYPE_STRING;
    }
    tab_panel->store = gtk_list_store_newv(tab_panel->column_count, types);

    GtkWidget *tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(tab_panel->store));
    g_object_unref(tab_panel->store); /* the tree view holds its own reference */
    for (int i = 0; i < tab_panel->column_count; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *column =
            gtk_tree_view_column_new_with_attributes(tab_panel->columns[i].label, renderer, "text", i, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);
    }

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), tree_view);
    tab_panel->page_widget = scroller;

    GtkWidget *label = gtk_label_new(tab_panel->title);
    GtkWidget *close_button = gtk_button_new_with_label("×");
    gtk_button_set_relief(GTK_BUTTON(close_button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close_button, FALSE);
    GtkWidget *label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(label_box), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(label_box), close_button, FALSE, FALSE, 0);

    g_object_set_data(G_OBJECT(scroller), "workbench-backend", backend);
    g_object_set_data(G_OBJECT(scroller), "workbench-tool-panel-tab", tab_panel);
    g_object_set_data(G_OBJECT(scroller), "workbench-tool-panel-label", label);
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_tool_panel_close_clicked), scroller);

    gtk_notebook_append_page(GTK_NOTEBOOK(backend->bottom_panel_notebook), scroller, label_box);
    gtk_widget_show_all(scroller);
    gtk_widget_show_all(label_box);

    g_ptr_array_add(backend->tool_panel_tabs, tab_panel);

    char data_dir[4096];
    g_strlcpy(data_dir, tab_panel->data_file_absolute_path, sizeof(data_dir));
    char *slash = strrchr(data_dir, '/');
    if (slash) {
        *slash = '\0';
    }
    /* Idempotent - see FileWatcher's own doc comment - so this is safe
     * even if another tool panel tab already watches the same directory. */
    file_watcher_watch_directory(backend->toolkit_watcher, data_dir);

    refresh_tool_panel_tab(tab_panel);
}

void tool_panel_handle_watch_event(GtkBackend *backend, const FileWatchEvent *event) {
    if (event->type == FILE_WATCH_DELETED) {
        return; /* leave the last-known rows in place rather than blanking the tab */
    }
    for (guint i = 0; i < backend->tool_panel_tabs->len; i++) {
        ToolPanelTab *tab_panel = g_ptr_array_index(backend->tool_panel_tabs, i);
        if (strcmp(tab_panel->data_file_toolkit_relative_path, event->new_relative_path) == 0) {
            refresh_tool_panel_tab(tab_panel);
        }
    }
}

void tool_panel_sync_running_state(GtkBackend *backend) {
    Workspace *workspace = workbench_get_workspace(backend->workbench);
    for (guint i = 0; i < backend->tool_panel_tabs->len; i++) {
        ToolPanelTab *tab_panel = g_ptr_array_index(backend->tool_panel_tabs, i);
        if (tab_panel->stopped) {
            continue;
        }
        Tab *tab = workspace_find_tab(workspace, tab_panel->terminal_tab_id);
        TerminalSession *session = tab ? (TerminalSession *)tab->backend_data : NULL;
        if (session && session->running) {
            continue;
        }
        tab_panel->stopped = TRUE;
        GtkWidget *label = g_object_get_data(G_OBJECT(tab_panel->page_widget), "workbench-tool-panel-label");
        if (label) {
            gchar *text = g_strdup_printf("%s (stopped)", tab_panel->title);
            gtk_label_set_text(GTK_LABEL(label), text);
            g_free(text);
        }
    }
}
/* --- end Manifest-driven bottom-panel tabs -------------------------------- */
