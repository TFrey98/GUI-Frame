#include "ui_gtk_backend.h"
#include "ui_gtk_tabs_internal.h"
#include "ui_gtk_editor_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* --- ui_gtk_editor_conflicts.c: watcher-driven external-change handling --- */

/* Generalizes a same-root rename to a possibly cross-root move -
 * old_root/new_root are the same pointer for a plain rename
 * (editor_handle_external_rename below), different pointers for a
 * Cut+Paste or drag-and-drop that crosses FILES/Toolkit. A no-op if
 * old_relative_path (under old_root) has no open editor/binary-info
 * tab. */
void editor_handle_external_move(GtkBackend *backend, const WorkspaceRoot *old_root, const char *old_relative_path,
                                  const WorkspaceRoot *new_root, const char *new_relative_path) {
    GtkWidget *page = find_file_tab(backend, old_root, old_relative_path);
    if (!page) {
        return;
    }

    Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
    EditorDocument *doc = tab->backend_data;
    doc->root = new_root;
    snprintf(doc->relative_path, sizeof(doc->relative_path), "%s", new_relative_path);
    const char *slash = strrchr(new_relative_path, '/');
    snprintf(doc->display_name, sizeof(doc->display_name), "%s", slash ? slash + 1 : new_relative_path);

    tab_set_title(tab, doc->display_name);
    update_tab_label_text(page);

    if (tab->type == TAB_TYPE_EDITOR) {
        GtkWidget *name_label = g_object_get_data(G_OBJECT(page), "workbench-editor-name-label");
        if (name_label) {
            gtk_label_set_text(GTK_LABEL(name_label), doc->display_name);
        }
    }
}

/* Called from ui_gtk_file_tree.c right after a real (non-create)
 * file_rename() succeeds. A no-op if old_relative_path has no open
 * editor/binary-info tab. */
void editor_handle_external_rename(GtkBackend *backend, const WorkspaceRoot *root, const char *old_relative_path,
                                    const char *new_relative_path) {
    editor_handle_external_move(backend, root, old_relative_path, root, new_relative_path);
}

void editor_handle_external_delete(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path) {
    GtkWidget *page = find_file_tab(backend, root, relative_path);
    if (!page) {
        return;
    }

    Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
    EditorDocument *doc = tab->backend_data;
    doc->deleted_on_disk = true;

    if (tab->type == TAB_TYPE_EDITOR) {
        GtkWidget *banner = g_object_get_data(G_OBJECT(page), "workbench-editor-deleted-banner");
        if (banner) {
            gtk_widget_set_visible(banner, TRUE);
        }
    }
}

/* Opens the on-disk content of doc's file as a separate, always-read-
 * only "<name> (on disk)" tab - a real, working action rather than a
 * stub, reading fresh from disk (never from doc's own in-memory buffer)
 * so it genuinely reflects the external change that triggered the
 * conflict dialog below. Deliberately bypasses open_or_focus_file_tab's
 * normal dedup (find_file_tab already skips any tab tagged
 * "workbench-editor-is-compare", see there) - Compare must never just
 * re-focus the already-open edited tab. */
static void open_compare_tab(GtkBackend *backend, const EditorDocument *doc) {
    EditorDocument *fresh = editor_document_open(doc->root, doc->relative_path, true, true);
    if (!fresh) {
        show_explorer_error(backend, "Could not read the on-disk version of this file.");
        return;
    }
    fresh->read_only = true;

    char compare_title[300];
    snprintf(compare_title, sizeof(compare_title), "%s (on disk)", fresh->display_name);

    Workspace *workspace = workbench_get_workspace(backend->workbench);
    Tab *tab = tab_create(TAB_TYPE_EDITOR, compare_title);
    tab->backend_data = fresh;
    workspace_add_tab(workspace, tab);
    add_tab_page(backend, tab, TRUE);

    /* add_tab_page() always appends to the notebook's end, so the page
     * it just built is the last one. */
    int last_index = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook)) - 1;
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), last_index);
    g_object_set_data(G_OBJECT(page), "workbench-editor-is-compare", GINT_TO_POINTER(TRUE));
}

/* --- External-modification conflict dialog -----------------------------
 * "<name> changed on disk." / Compare / Reload from Disk / Keep Editor
 * Version - shown only when the file changed externally *and* the open
 * tab has real unsaved edits (see editor_handle_external_modification
 * below; an unedited tab reloads silently instead, no dialog needed). */

static void on_conflict_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    EditorPageContext *ctx = user_data;
    Tab *tab = g_object_get_data(G_OBJECT(ctx->page), "workbench-tab");
    EditorDocument *doc = tab->backend_data;

    if (response_id == GTK_RESPONSE_APPLY) { /* Compare */
        open_compare_tab(ctx->backend, doc);
    } else if (response_id == GTK_RESPONSE_YES) { /* Reload from Disk */
        reload_editor_document_from_disk(ctx->backend, ctx->page);
    }
    /* Compare, "Keep Editor Version", and dismissing the dialog outright
     * all leave the buffer untouched - just clear the flag so it isn't
     * left permanently set. Reload already clears it too, via
     * reload_editor_document_from_disk, but clearing it again here is
     * harmless. */
    doc->externally_modified = false;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void open_external_change_conflict_dialog(GtkBackend *backend, GtkWidget *page) {
    Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
    EditorPageContext *ctx = g_object_get_data(G_OBJECT(page), "workbench-editor-page-context");

    GtkWindow *parent = gtk_application_get_active_window(backend->gtk_app);
    GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                                "%s changed on disk.", tab->title);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Compare", GTK_RESPONSE_APPLY);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Reload from Disk", GTK_RESPONSE_YES);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Keep Editor Version", GTK_RESPONSE_NO);
    g_signal_connect(dialog, "response", G_CALLBACK(on_conflict_response), ctx);
    gtk_widget_show_all(dialog);
}

void editor_handle_external_modification(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path) {
    GtkWidget *page = find_file_tab(backend, root, relative_path);
    if (!page) {
        return;
    }
    Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
    if (tab->type != TAB_TYPE_EDITOR) {
        return; /* a binary-info tab has no live buffer to reload/compare */
    }
    EditorDocument *doc = tab->backend_data;

    char resolved[4096];
    if (!workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
        return;
    }
    struct stat st;
    if (stat(resolved, &st) != 0) {
        return; /* shouldn't happen for a just-reported MODIFIED event, but stay defensive */
    }

    /* An equal mtime means this event is just an echo of this app's own
     * save (the safe-write's rename() itself triggers the watched
     * directory's own IN_CLOSE_WRITE/IN_MOVED_TO) - ignored rather than
     * reloaded, so saving never causes a spurious reload or prompt. */
    if (st.st_mtim.tv_sec == doc->last_known_mtime.tv_sec && st.st_mtim.tv_nsec == doc->last_known_mtime.tv_nsec) {
        return;
    }

    if (!doc->modified) {
        reload_editor_document_from_disk(backend, page);
        return;
    }

    doc->externally_modified = true;
    open_external_change_conflict_dialog(backend, page);
}
