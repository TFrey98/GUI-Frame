#include "ui_gtk_backend.h"
#include "ui_gtk_explorer_internal.h"

#include <string.h>

/* --- Reveal in Explorer ----------------------------------------------------
 * Backs a search result's folder activation (ui_gtk_search.c) - VS
 * Code's own "Reveal in Explorer." */

void reveal_in_explorer(GtkBackend *backend, int source, const char *relative_path) {
    GtkTreeStore *store = backend->explorer_store;
    GtkTreeView *tree_view = GTK_TREE_VIEW(backend->explorer_tree_view);

    GtkTreeIter current;
    if (!explorer_permanent_root_iter(store, source, &current)) {
        return;
    }

    if (relative_path[0] != '\0') {
        char path_copy[4096];
        snprintf(path_copy, sizeof(path_copy), "%s", relative_path);
        char *saveptr = NULL;
        char *segment = strtok_r(path_copy, "/", &saveptr);
        while (segment) {
            gboolean loaded = FALSE;
            gtk_tree_model_get(GTK_TREE_MODEL(store), &current, EXPLORER_COL_LOADED, &loaded, -1);
            if (!loaded) {
                load_row_children(backend, store, &current);
            }
            GtkTreeIter child;
            if (!explorer_find_child_by_name(GTK_TREE_MODEL(store), &current, segment, &child)) {
                return; /* out of sync with disk (e.g. deleted since the search ran) - bail quietly */
            }
            current = child;
            segment = strtok_r(NULL, "/", &saveptr);
        }
    }

    GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(store), &current);
    gtk_tree_view_expand_to_path(tree_view, path);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(tree_view), path);
    gtk_tree_view_scroll_to_cell(tree_view, path, NULL, TRUE, 0.5, 0.0);
    gtk_tree_path_free(path);
}
/* --- end Reveal in Explorer -------------------------------------------------- */
