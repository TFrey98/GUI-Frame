#include "ui_gtk_backend.h"
#include "ui_gtk_explorer_internal.h"
#include "ui_gtk_editor_internal.h"

#include <stdio.h>

#include "files/file_search.h"

/* --- Search window -----------------------------------------------------
 * A plain non-modal GtkWindow (not a transactional GtkDialog like New
 * Listener/Properties/Save As) - Search stays open across multiple
 * searches and result clicks, closer in spirit to a tool panel.
 * Singleton per app run: backend->search_window tracks it so a second
 * click on the top bar's Search button presents the existing one rather
 * than opening a duplicate. */

#define SEARCH_MAX_MATCHES 500

enum {
    SEARCH_COL_DISPLAY_PATH, /* "TOOLBOX/relative_path" or "Toolkit/relative_path" */
    SEARCH_COL_SOURCE,
    SEARCH_COL_PATH,
    SEARCH_COL_IS_DIR,
    SEARCH_COL_COUNT
};

typedef struct SearchContext {
    GtkBackend *backend;
    GtkWidget *entry;
    GtkWidget *match_case;
    GtkWidget *status_label;
    GtkListStore *store;
} SearchContext;

/* Always leaves a definite answer in status_label - "no query typed",
 * "Searching..." never lingers unanswered since the scan is synchronous,
 * and "0 items" is explicit rather than an empty list that looks
 * identical to "the search never ran." */
static void run_search(SearchContext *ctx) {
    gtk_list_store_clear(ctx->store);

    const char *query = gtk_entry_get_text(GTK_ENTRY(ctx->entry));
    if (query[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(ctx->status_label), "Type a query, then press Enter or click Search.");
        return;
    }
    gboolean case_sensitive = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctx->match_case));

    static const int sources[] = {EXPLORER_SOURCE_FILES, EXPLORER_SOURCE_TOOLKIT};
    static const char *const labels[] = {"TOOLBOX", "Toolkit"};

    int total = 0;
    FileSearchMatch *matches = g_new(FileSearchMatch, SEARCH_MAX_MATCHES);
    for (size_t s = 0; s < G_N_ELEMENTS(sources); s++) {
        const WorkspaceRoot *root = explorer_root_for_source(ctx->backend, sources[s]);
        int n = file_search_scan(root, query, case_sensitive, matches, SEARCH_MAX_MATCHES);
        total += n;
        for (int i = 0; i < n; i++) {
            char display[4400];
            snprintf(display, sizeof(display), "%s/%s", labels[s], matches[i].relative_path);

            GtkTreeIter iter;
            gtk_list_store_append(ctx->store, &iter);
            gtk_list_store_set(ctx->store, &iter, SEARCH_COL_DISPLAY_PATH, display, SEARCH_COL_SOURCE, sources[s],
                                SEARCH_COL_PATH, matches[i].relative_path, SEARCH_COL_IS_DIR, matches[i].is_dir, -1);
        }
    }
    g_free(matches);

    char status_text[256];
    if (total == 0) {
        snprintf(status_text, sizeof(status_text), "No items found matching \xE2\x80\x9C%s\xE2\x80\x9D.", query);
    } else if (total == 1) {
        snprintf(status_text, sizeof(status_text), "1 item found matching \xE2\x80\x9C%s\xE2\x80\x9D.", query);
    } else {
        snprintf(status_text, sizeof(status_text), "%d items found matching \xE2\x80\x9C%s\xE2\x80\x9D.", total,
                 query);
    }
    gtk_label_set_text(GTK_LABEL(ctx->status_label), status_text);
}

static void on_search_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)entry;
    run_search((SearchContext *)user_data);
}

static void on_search_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    run_search((SearchContext *)user_data);
}

static void on_search_result_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column,
                                        gpointer user_data) {
    (void)column;
    GtkBackend *backend = user_data;
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
        return;
    }

    int source = EXPLORER_SOURCE_FILES;
    gchar *relative_path = NULL;
    gboolean is_dir = FALSE;
    gtk_tree_model_get(model, &iter, SEARCH_COL_SOURCE, &source, SEARCH_COL_PATH, &relative_path, SEARCH_COL_IS_DIR,
                        &is_dir, -1);
    if (!relative_path) {
        return;
    }

    if (is_dir) {
        reveal_in_explorer(backend, source, relative_path);
    } else {
        const WorkspaceRoot *root = explorer_root_for_source(backend, source);
        /* Search results never come from a pre-loaded FileTreeNode (the
         * scan walks the real filesystem directly, not FileTree's lazy
         * registry) - explorer_toolkit_file_flags() is the general-
         * purpose stat-based flag helper every Toolkit row already uses
         * for the exact same reason, and works identically for either
         * source. */
        char resolved[4096];
        bool executable = false, read_only = true;
        if (workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
            explorer_toolkit_file_flags(resolved, &executable, &read_only);
        }
        open_or_focus_file_tab(backend, root, relative_path, executable, read_only);
    }
    g_free(relative_path);
}

static void on_search_window_destroy(GtkWidget *window, gpointer user_data) {
    (void)window;
    GtkBackend *backend = user_data;
    backend->search_window = NULL;
}

void open_or_present_search_window(GtkBackend *backend) {
    if (backend->search_window) {
        gtk_window_present(GTK_WINDOW(backend->search_window));
        return;
    }

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Search");
    gtk_window_set_default_size(GTK_WINDOW(window), 480, 400);
    gtk_window_set_transient_for(GTK_WINDOW(window), gtk_application_get_active_window(backend->gtk_app));
    /* Registered with the application so it shares the same "quit once
     * every window is gone" lifecycle the main window already has -
     * otherwise closing the main window would leave this one as an
     * orphan keeping the process alive. */
    gtk_application_add_window(backend->gtk_app, GTK_WINDOW(window));

    GtkWidget *root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(root_box), 8);

    GtkWidget *search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Search file and folder names\xE2\x80\xA6");
    gtk_widget_set_hexpand(entry, TRUE);
    g_object_set_data(G_OBJECT(entry), "workbench-search-entry", entry);
    GtkWidget *match_case = gtk_check_button_new_with_label("Match Case");
    g_object_set_data(G_OBJECT(match_case), "workbench-search-match-case", match_case);
    GtkWidget *search_button = gtk_button_new_with_label("Search");
    g_object_set_data(G_OBJECT(search_button), "workbench-search-button", search_button);
    gtk_box_pack_start(GTK_BOX(search_row), entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(search_row), match_case, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search_row), search_button, FALSE, FALSE, 0);

    GtkWidget *status_label = gtk_label_new("Type a query, then press Enter or click Search.");
    gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
    g_object_set_data(G_OBJECT(status_label), "workbench-search-status-label", status_label);

    GtkListStore *store = gtk_list_store_new(SEARCH_COL_COUNT, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING,
                                              G_TYPE_BOOLEAN);
    GtkWidget *tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store); /* the tree view holds its own reference */
    g_object_set_data(G_OBJECT(tree_view), "workbench-search-results-tree", tree_view);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree_view), TRUE);

    GtkTreeViewColumn *path_col = gtk_tree_view_column_new_with_attributes(
        "Location", gtk_cell_renderer_text_new(), "text", SEARCH_COL_DISPLAY_PATH, NULL);
    gtk_tree_view_column_set_expand(path_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), path_col);
    g_signal_connect(tree_view, "row-activated", G_CALLBACK(on_search_result_activated), backend);

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), tree_view);
    gtk_widget_set_vexpand(scroller, TRUE);

    SearchContext *ctx = g_new(SearchContext, 1);
    ctx->backend = backend;
    ctx->entry = entry;
    ctx->match_case = match_case;
    ctx->status_label = status_label;
    ctx->store = store;
    g_object_set_data_full(G_OBJECT(window), "workbench-search-context", ctx, g_free);
    g_signal_connect(entry, "activate", G_CALLBACK(on_search_entry_activate), ctx);
    g_signal_connect(search_button, "clicked", G_CALLBACK(on_search_button_clicked), ctx);

    gtk_box_pack_start(GTK_BOX(root_box), search_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root_box), status_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root_box), scroller, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(window), root_box);

    backend->search_window = window;
    g_signal_connect(window, "destroy", G_CALLBACK(on_search_window_destroy), backend);

    gtk_widget_show_all(window);
    gtk_widget_grab_focus(entry);
}
/* --- end Search window --------------------------------------------------- */
