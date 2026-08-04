#include "ui_gtk_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Bundles what every Save/Save As/Revert/buffer-changed handler on an
 * editor page needs - the page itself (to reach its Tab/EditorDocument
 * and its own widgets via g_object_get_data) plus backend (for the
 * WorkspaceRoot and for showing an error dialog). Tagged once per page
 * in build_editor_page, freed automatically with the page. */
typedef struct EditorPageContext {
    GtkBackend *backend;
    GtkWidget *page;
} EditorPageContext;

/* Re-syncs Save/Revert sensitivity (only meaningful once doc->modified
 * changes) and the notebook tab's own label (the modified dot) -
 * called after every state change: a keystroke, a successful save, a
 * revert, or a Save As. */
static void refresh_editor_page_buttons(GtkWidget *page) {
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    EditorDocument *doc = tab->backend_data;
    GtkWidget *save_button = g_object_get_data(G_OBJECT(page), "toolbox-editor-save-button");
    GtkWidget *revert_button = g_object_get_data(G_OBJECT(page), "toolbox-editor-revert-button");
    if (save_button) {
        gtk_widget_set_sensitive(save_button, doc->modified);
    }
    if (revert_button) {
        gtk_widget_set_sensitive(revert_button, doc->modified);
    }
    update_tab_label_text(page);
}

/* Marks the document modified on any buffer change - connected *after*
 * the initial gtk_text_buffer_set_text() load in build_editor_page, so
 * the load itself never marks a freshly-opened document modified (same
 * "set defaults, then connect" ordering the New Listener dialog's own
 * "changed" handlers already use). */
static void on_editor_buffer_changed(GtkTextBuffer *buffer, gpointer user_data) {
    (void)buffer;
    EditorPageContext *ctx = user_data;
    Tab *tab = g_object_get_data(G_OBJECT(ctx->page), "toolbox-tab");
    EditorDocument *doc = tab->backend_data;
    doc->modified = TRUE;
    refresh_editor_page_buttons(ctx->page);
}

EditorSaveResult save_editor_page(GtkBackend *backend, GtkWidget *page) {
    (void)backend; /* doc knows its own root now - see EditorDocument.root */
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    EditorDocument *doc = tab->backend_data;
    GtkWidget *view = g_object_get_data(G_OBJECT(page), "toolbox-editor-text-view");
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    EditorSaveResult result = editor_document_save(doc->root, doc, NULL, text, strlen(text));
    g_free(text);

    if (result == EDITOR_SAVE_OK) {
        /* A save writes the file back out regardless of whether it had
         * been externally deleted or modified - clear both flags and
         * hide the banner rather than requiring the user to separately
         * dismiss them. */
        doc->deleted_on_disk = false;
        doc->externally_modified = false;
        GtkWidget *banner = g_object_get_data(G_OBJECT(page), "toolbox-editor-deleted-banner");
        if (banner) {
            gtk_widget_set_visible(banner, FALSE);
        }
        refresh_editor_page_buttons(page);
    }
    return result;
}

/* --- Save As dialog ---------------------------------------------------
 * A small path-entry dialog (add_form_row-based, matching the New
 * Listener dialog's own shape) rather than a native GtkFileChooser -
 * this app confines everything to a single WorkspaceRoot by design, so
 * "Save As" targets a path within it, not an arbitrary filesystem
 * location. */

typedef struct SaveAsDialogState {
    GtkBackend *backend;
    GtkWidget *page;
    GtkWidget *path_entry;
    GtkWidget *error_label;
} SaveAsDialogState;

static void on_save_as_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    SaveAsDialogState *state = user_data;
    if (response_id != GTK_RESPONSE_OK) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }

    GtkWidget *page = state->page;
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    EditorDocument *doc = tab->backend_data;
    GtkWidget *view = g_object_get_data(G_OBJECT(page), "toolbox-editor-text-view");
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    const char *new_path = gtk_entry_get_text(GTK_ENTRY(state->path_entry));
    EditorSaveResult result = editor_document_save(doc->root, doc, new_path, text, strlen(text));
    g_free(text);

    if (result != EDITOR_SAVE_OK) {
        gtk_label_set_text(GTK_LABEL(state->error_label), editor_save_error_message(result));
        return; /* leave the dialog open so the user can fix it */
    }

    tab_set_title(tab, doc->display_name);
    GtkWidget *name_label = g_object_get_data(G_OBJECT(page), "toolbox-editor-name-label");
    if (name_label) {
        gtk_label_set_text(GTK_LABEL(name_label), doc->display_name);
    }
    refresh_editor_page_buttons(page);
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void open_save_as_dialog(GtkBackend *backend, GtkWidget *page, GtkWindow *parent) {
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    EditorDocument *doc = tab->backend_data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Save As", parent, GTK_DIALOG_MODAL, "_Cancel",
                                                      GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_OK, NULL);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid, TRUE, TRUE, 0);

    SaveAsDialogState *state = g_new0(SaveAsDialogState, 1);
    state->backend = backend;
    state->page = page;

    state->path_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(state->path_entry), doc->relative_path);
    add_form_row(GTK_GRID(grid), 0, "Save As", state->path_entry);
    state->error_label = add_error_row(GTK_GRID(grid), 1);

    g_signal_connect(dialog, "response", G_CALLBACK(on_save_as_response), state);
    g_object_set_data_full(G_OBJECT(dialog), "toolbox-save-as-dialog-state", state, g_free);
    g_object_set_data(G_OBJECT(state->path_entry), "toolbox-save-as-path-entry", state->path_entry);

    gtk_widget_show_all(dialog);
}

static void on_editor_save_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    EditorPageContext *ctx = user_data;
    EditorSaveResult result = save_editor_page(ctx->backend, ctx->page);
    if (result != EDITOR_SAVE_OK) {
        show_explorer_error(ctx->backend, editor_save_error_message(result));
    }
}

static void on_editor_save_as_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    EditorPageContext *ctx = user_data;
    GtkWidget *toplevel = gtk_widget_get_toplevel(ctx->page);
    open_save_as_dialog(ctx->backend, ctx->page, GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL);
}

/* --- Revert ------------------------------------------------------------- */

/* The one reload primitive both a confirmed Revert-button click and an
 * unedited external-modification event (see editor_handle_external_
 * modification below) share - re-reads the file fresh from disk into
 * the already-open page's buffer/EditorDocument, clearing modified/
 * externally_modified and re-stamping last_known_mtime so a save-echo
 * comparison against this fresh read starts clean. Shows an error and
 * leaves everything untouched if the file can no longer be read (e.g.
 * deleted out from under a Revert click). */
static void reload_editor_document_from_disk(GtkBackend *backend, GtkWidget *page) {
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    EditorDocument *doc = tab->backend_data;

    EditorDocument *fresh = editor_document_open(doc->root, doc->relative_path, doc->read_only, true);
    if (!fresh) {
        show_explorer_error(backend, "Could not reload this file from disk.");
        return;
    }

    GtkWidget *view = g_object_get_data(G_OBJECT(page), "toolbox-editor-text-view");
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    EditorPageContext *ctx = g_object_get_data(G_OBJECT(page), "toolbox-editor-page-context");
    /* Blocked so this programmatic reset doesn't itself re-mark the doc
     * modified via on_editor_buffer_changed - same reasoning as the
     * initial "set defaults, then connect" load ordering. */
    g_signal_handlers_block_by_func(buffer, on_editor_buffer_changed, ctx);
    gtk_text_buffer_set_text(buffer, fresh->contents ? fresh->contents : "", (gint)fresh->content_size);
    g_signal_handlers_unblock_by_func(buffer, on_editor_buffer_changed, ctx);

    free(doc->contents);
    doc->contents = fresh->contents;
    doc->content_size = fresh->content_size;
    doc->modified = false;
    doc->externally_modified = false;
    doc->deleted_on_disk = false;
    doc->last_known_mtime = fresh->last_known_mtime;
    fresh->contents = NULL; /* ownership moved above - prevent a double-free below */
    editor_document_destroy(fresh);

    GtkWidget *banner = g_object_get_data(G_OBJECT(page), "toolbox-editor-deleted-banner");
    if (banner) {
        gtk_widget_set_visible(banner, FALSE);
    }
    refresh_editor_page_buttons(page);
}

static void on_revert_confirm_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    EditorPageContext *ctx = user_data;
    if (response_id == GTK_RESPONSE_YES) {
        reload_editor_document_from_disk(ctx->backend, ctx->page);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

/* Only reachable while the Revert button is sensitive (doc->modified),
 * so every real click here is a genuine "discard my changes" - worth
 * confirming, unlike Save/Save As which never discard anything. */
static void on_editor_revert_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    EditorPageContext *ctx = user_data;
    Tab *tab = g_object_get_data(G_OBJECT(ctx->page), "toolbox-tab");

    GtkWindow *parent = gtk_application_get_active_window(ctx->backend->gtk_app);
    GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                                "Revert changes to %s?", tab->title);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Revert", GTK_RESPONSE_YES);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Cancel", GTK_RESPONSE_CANCEL);
    g_signal_connect(dialog, "response", G_CALLBACK(on_revert_confirm_response), ctx);
    gtk_widget_show_all(dialog);
}

GtkWidget *build_editor_page(GtkBackend *backend, Tab *tab) {
    EditorDocument *doc = tab->backend_data;

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(page), 8);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *name_label = gtk_label_new(doc->display_name);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
    gtk_box_pack_start(GTK_BOX(header), name_label, TRUE, TRUE, 0);

    EditorPageContext *ctx = g_new(EditorPageContext, 1);
    ctx->backend = backend;
    ctx->page = page;
    g_object_set_data_full(G_OBJECT(page), "toolbox-editor-page-context", ctx, g_free);

    /* Save/Save As/Revert never make sense for a read-only doc (a
     * chmod'd file, or one truncated by EDITOR_DOCUMENT_MAX_SIZE) - the
     * text view is already non-editable for the same reason, so these
     * stay fully hidden rather than merely insensitive. no_show_all is
     * required here (not just set_visible) since add_tab_page's own
     * gtk_widget_show_all() runs *after* this function returns and
     * would otherwise force them visible regardless - the same
     * "show_all forces every child visible" gotcha the New Listener
     * dialog already documents. */
    GtkWidget *save_button = gtk_button_new_with_label("Save");
    GtkWidget *save_as_button = gtk_button_new_with_label("Save As");
    GtkWidget *revert_button = gtk_button_new_with_label("Revert");
    g_signal_connect(save_button, "clicked", G_CALLBACK(on_editor_save_clicked), ctx);
    g_signal_connect(save_as_button, "clicked", G_CALLBACK(on_editor_save_as_clicked), ctx);
    g_signal_connect(revert_button, "clicked", G_CALLBACK(on_editor_revert_clicked), ctx);
    gboolean writable = !doc->read_only;
    gtk_widget_set_no_show_all(save_button, TRUE);
    gtk_widget_set_no_show_all(save_as_button, TRUE);
    gtk_widget_set_no_show_all(revert_button, TRUE);
    gtk_widget_set_visible(save_button, writable);
    gtk_widget_set_visible(save_as_button, writable);
    gtk_widget_set_visible(revert_button, writable);
    gtk_widget_set_sensitive(save_button, FALSE);
    gtk_widget_set_sensitive(revert_button, FALSE);
    gtk_box_pack_start(GTK_BOX(header), save_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), save_as_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), revert_button, FALSE, FALSE, 0);

    /* The doc's "obvious read-only indicator" - a visible label, on top
     * of gtk_text_view_set_editable() below actually enforcing it at
     * the widget level, not just displaying it. Same no_show_all
     * requirement as the buttons above - this one was previously
     * missing it (a latent cosmetic bug: the label was always forced
     * visible by add_tab_page's later show_all(), regardless of
     * doc->read_only, since nothing before Step 4 ever exercised the
     * non-read-only negative case). */
    GtkWidget *read_only_label = gtk_label_new("Read-only");
    gtk_widget_set_no_show_all(read_only_label, TRUE);
    gtk_widget_set_visible(read_only_label, doc->read_only);
    gtk_box_pack_start(GTK_BOX(header), read_only_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), header, FALSE, FALSE, 0);

    /* Hidden until editor_handle_external_delete() sets it - same
     * no_show_all-hidden-until-true pattern as read_only_label above. */
    GtkWidget *deleted_banner = gtk_label_new("Deleted from disk");
    gtk_widget_set_no_show_all(deleted_banner, TRUE);
    gtk_widget_set_visible(deleted_banner, FALSE);
    gtk_box_pack_start(GTK_BOX(page), deleted_banner, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(page), "toolbox-editor-deleted-banner", deleted_banner);

    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), !doc->read_only);

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buffer, doc->contents ? doc->contents : "", (gint)doc->content_size);
    g_signal_connect(buffer, "changed", G_CALLBACK(on_editor_buffer_changed), ctx);

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroller), view);
    gtk_box_pack_start(GTK_BOX(page), scroller, TRUE, TRUE, 0);

    g_object_set_data(G_OBJECT(page), "toolbox-editor-text-view", view);
    g_object_set_data(G_OBJECT(page), "toolbox-editor-read-only-label", read_only_label);
    g_object_set_data(G_OBJECT(page), "toolbox-editor-name-label", name_label);
    g_object_set_data(G_OBJECT(page), "toolbox-editor-save-button", save_button);
    g_object_set_data(G_OBJECT(page), "toolbox-editor-save-as-button", save_as_button);
    g_object_set_data(G_OBJECT(page), "toolbox-editor-revert-button", revert_button);

    return page;
}

GtkWidget *build_binary_info_page(GtkBackend *backend, Tab *tab) {
    (void)backend;
    EditorDocument *doc = tab->backend_data;

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);

    char size_text[32];
    snprintf(size_text, sizeof(size_text), "%zu bytes", doc->content_size);

    int row = 0;
    add_form_row(GTK_GRID(grid), row++, "Name", gtk_label_new(doc->display_name));
    add_form_row(GTK_GRID(grid), row++, "Path", gtk_label_new(doc->relative_path));
    add_form_row(GTK_GRID(grid), row++, "Size", gtk_label_new(size_text));
    add_form_row(GTK_GRID(grid), row++, "Read-only", gtk_label_new(doc->read_only ? "Yes" : "No"));

    GtkWidget *notice = gtk_label_new("This file appears to be binary and can't be edited.");
    gtk_label_set_xalign(GTK_LABEL(notice), 0.0);
    gtk_box_pack_start(GTK_BOX(page), notice, FALSE, FALSE, 0);

    g_object_set_data(G_OBJECT(page), "toolbox-binary-info-page", page);

    return page;
}

/* Matches on (root, relative_path) rather than relative_path alone -
 * the app now has more than one root (files/ and toolkit/), and a
 * relative_path match alone could collide if a TOOLBOX file and a
 * Toolkit file ever happened to share the same relative path. Pointer
 * equality on root is safe and correct: both roots are long-lived
 * values owned by App, never copied or relocated. */
static GtkWidget *find_file_tab(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path) {
    if (!backend->notebook) {
        return NULL;
    }
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i);
        /* A Compare tab (see open_compare_tab below) deliberately shares
         * its (root, relative_path) with the real edited tab it was
         * opened alongside - it must never be mistaken for a match here,
         * or a later open/rename/delete could end up acting on the
         * read-only snapshot instead of the real open document. */
        if (g_object_get_data(G_OBJECT(page), "toolbox-editor-is-compare")) {
            continue;
        }
        Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
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
     * "Symlink -> open only if target remains inside toolbox": it
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

    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    EditorDocument *doc = tab->backend_data;
    doc->root = new_root;
    snprintf(doc->relative_path, sizeof(doc->relative_path), "%s", new_relative_path);
    const char *slash = strrchr(new_relative_path, '/');
    snprintf(doc->display_name, sizeof(doc->display_name), "%s", slash ? slash + 1 : new_relative_path);

    tab_set_title(tab, doc->display_name);
    update_tab_label_text(page);

    if (tab->type == TAB_TYPE_EDITOR) {
        GtkWidget *name_label = g_object_get_data(G_OBJECT(page), "toolbox-editor-name-label");
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

    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    EditorDocument *doc = tab->backend_data;
    doc->deleted_on_disk = true;

    if (tab->type == TAB_TYPE_EDITOR) {
        GtkWidget *banner = g_object_get_data(G_OBJECT(page), "toolbox-editor-deleted-banner");
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
 * "toolbox-editor-is-compare", see there) - Compare must never just
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
    g_object_set_data(G_OBJECT(page), "toolbox-editor-is-compare", GINT_TO_POINTER(TRUE));
}

/* --- External-modification conflict dialog -----------------------------
 * "<name> changed on disk." / Compare / Reload from Disk / Keep Editor
 * Version - shown only when the file changed externally *and* the open
 * tab has real unsaved edits (see editor_handle_external_modification
 * below; an unedited tab reloads silently instead, no dialog needed). */

static void on_conflict_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    EditorPageContext *ctx = user_data;
    Tab *tab = g_object_get_data(G_OBJECT(ctx->page), "toolbox-tab");
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
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    EditorPageContext *ctx = g_object_get_data(G_OBJECT(page), "toolbox-editor-page-context");

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
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
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

void save_all_modified_editors(GtkBackend *backend) {
    if (!backend->notebook) {
        return;
    }

    GString *failures = g_string_new(NULL);
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
        if (!tab || tab->type != TAB_TYPE_EDITOR) {
            continue;
        }
        EditorDocument *doc = tab->backend_data;
        if (doc->read_only || !doc->modified) {
            continue;
        }
        EditorSaveResult result = save_editor_page(backend, page);
        if (result != EDITOR_SAVE_OK) {
            if (failures->len > 0) {
                g_string_append_c(failures, '\n');
            }
            g_string_append_printf(failures, "%s: %s", doc->display_name, editor_save_error_message(result));
        }
    }

    /* The doc's explicit "Save All reports each failure rather than
     * silently skipping files" - one summary dialog listing every
     * failure, not one per file and not just the first. */
    if (failures->len > 0) {
        gchar *message = g_strdup_printf("Some files could not be saved:\n\n%s", failures->str);
        show_explorer_error(backend, message);
        g_free(message);
    }
    g_string_free(failures, TRUE);
}

static GtkWidget *active_editor_page(GtkBackend *backend) {
    if (!backend->notebook) {
        return NULL;
    }
    gint idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(backend->notebook));
    if (idx < 0) {
        return NULL;
    }
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), idx);
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    if (!tab || tab->type != TAB_TYPE_EDITOR) {
        return NULL;
    }
    EditorDocument *doc = tab->backend_data;
    return doc->read_only ? NULL : page;
}

void trigger_save_active_editor(GtkBackend *backend) {
    GtkWidget *page = active_editor_page(backend);
    if (!page) {
        return;
    }
    EditorSaveResult result = save_editor_page(backend, page);
    if (result != EDITOR_SAVE_OK) {
        show_explorer_error(backend, editor_save_error_message(result));
    }
}

void trigger_save_as_active_editor(GtkBackend *backend, GtkWindow *window) {
    GtkWidget *page = active_editor_page(backend);
    if (!page) {
        return;
    }
    open_save_as_dialog(backend, page, window);
}
