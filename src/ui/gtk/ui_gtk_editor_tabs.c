#include "ui_gtk_backend.h"
#include "ui_gtk_tabs_internal.h"
#include "ui_gtk_editor_internal.h"

#include <string.h>

/* --- ui_gtk_editor_tabs.c: open-or-focus dedup for editor/binary-info tabs --- */

/* Matches on (root, relative_path) rather than relative_path alone -
 * the app now has more than one root (files/ and toolkit/), and a
 * relative_path match alone could collide if a TOOLBOX file and a
 * Toolkit file ever happened to share the same relative path. Pointer
 * equality on root is safe and correct: both roots are long-lived
 * values owned by App, never copied or relocated. Promoted (was
 * static) - ui_gtk_editor_conflicts.c's external-change handlers need
 * it too. */
GtkWidget *find_file_tab(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path) {
    if (!backend->notebook) {
        return NULL;
    }
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i);
        /* A Compare tab (see ui_gtk_editor_conflicts.c's open_compare_tab)
         * deliberately shares its (root, relative_path) with the real
         * edited tab it was opened alongside - it must never be
         * mistaken for a match here, or a later open/rename/delete
         * could end up acting on the read-only snapshot instead of the
         * real open document. */
        if (g_object_get_data(G_OBJECT(page), "workbench-editor-is-compare")) {
            continue;
        }
        Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
        if (!tab || (tab->type != TAB_TYPE_EDITOR && tab->type != TAB_TYPE_BINARY_INFO)) {
            continue;
        }
        const EditorDocument *doc = tab->backend_data;
        if (doc && doc->root == root && strcmp(doc->relative_path, relative_path) == 0) {
            return page;
        }
    }
    return NULL;
}

void open_or_focus_file_tab(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path,
                             bool executable, bool read_only) {
    /* "Does not open duplicate tabs" - checked before touching disk at
     * all, matching open_or_focus_connection_terminal's own order. */
    GtkWidget *existing = find_file_tab(backend, root, relative_path);
    if (existing) {
        focus_page(backend, existing);
        return;
    }

    /* Reusing workspace_root_resolve_path() here is what satisfies
     * "Symlink -> open only if target remains inside workbench": it
     * already resolves through symlinks via realpath() and enforces
     * containment - no separate symlink-specific check needed. */
    char resolved[4096];
    if (!workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
        show_explorer_error(backend, "Can't open - the target is missing or outside the workspace.");
        return;
    }

    FileClassification classification = file_classify(resolved, executable);
    if (classification.target == FILE_TARGET_UNSUPPORTED) {
        show_explorer_error(backend, "This file can't be opened.");
        return;
    }

    bool load_contents = classification.target == FILE_TARGET_EDITOR;
    EditorDocument *doc = editor_document_open(root, relative_path, read_only, load_contents);
    if (!doc) {
        show_explorer_error(backend, "Could not read this file.");
        return;
    }

    Workspace *workspace = workbench_get_workspace(backend->workbench);
    Tab *tab = tab_create(load_contents ? TAB_TYPE_EDITOR : TAB_TYPE_BINARY_INFO, doc->display_name);
    tab->backend_data = doc;
    workspace_add_tab(workspace, tab);
    add_tab_page(backend, tab, TRUE);
}
