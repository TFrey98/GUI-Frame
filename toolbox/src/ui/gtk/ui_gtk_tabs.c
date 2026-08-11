#include "ui_gtk_backend.h"
#include "ui_gtk_tabs_internal.h"
#include "ui_gtk_editor_internal.h"
#include "ui_gtk_terminal_internal.h"

/* --- Generic notebook/tab lifecycle ---------------------------------------
 * Tab creation, focus, and destruction shared by every tab type. Tab
 * label construction/rename/context-menu lives in ui_gtk_tab_labels.c;
 * close confirmation in ui_gtk_tab_close.c; the listener console tab in
 * ui_gtk_listener_page.c - all built on add_tab_page()/focus_page()/
 * close_tab_page() here. */

static void remove_terminal_entry(GtkBackend *backend, Terminal *view) {
    for (guint i = 0; i < backend->terminal_entries->len; i++) {
        TerminalEntry *entry = g_ptr_array_index(backend->terminal_entries, i);
        if (entry->view == view) {
            g_ptr_array_remove_index_fast(backend->terminal_entries, i);
            g_free(entry);
            return;
        }
    }
}

/* The actual close logic - shared by every tab type/state, unconditional
 * once called. The listener-running/modified-editor confirmation that
 * can intercept *before* this runs lives in ui_gtk_tab_close.c. */
void close_tab_page(GtkWidget *page) {
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    Workspace *workspace = g_object_get_data(G_OBJECT(page), "toolbox-workspace");
    GtkWidget *notebook = gtk_widget_get_ancestor(page, GTK_TYPE_NOTEBOOK);

    /* Captured before workspace_close_tab, which destroys *tab - tab_id and
     * page_num must survive that call for the workspace/notebook cleanup below. */
    uint64_t tab_id = tab->id;
    int page_num = gtk_notebook_page_num(GTK_NOTEBOOK(notebook), page);

    if (tab->type == TAB_TYPE_TERMINAL) {
        GtkBackend *backend = g_object_get_data(G_OBJECT(page), "toolbox-backend");
        Terminal *view = g_object_get_data(G_OBJECT(page), "toolbox-view");
        if (view) {
            remove_terminal_entry(backend, view);
            terminal_destroy(view);
        }
        if (tab->backend_data) {
            terminal_session_destroy((TerminalSession *)tab->backend_data);
            tab->backend_data = NULL;
        }
    } else if (tab->type == TAB_TYPE_EDITOR || tab->type == TAB_TYPE_BINARY_INFO) {
        editor_document_destroy((EditorDocument *)tab->backend_data);
        tab->backend_data = NULL;
    }

    workspace_close_tab(workspace, tab_id);
    if (page_num >= 0) {
        gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), page_num);
    }
}

void update_tab_label_text(GtkWidget *page) {
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    GtkWidget *label = g_object_get_data(G_OBJECT(page), "toolbox-tab-label-widget");
    if (!tab || !label) {
        return;
    }
    gboolean modified = tab->type == TAB_TYPE_EDITOR && ((EditorDocument *)tab->backend_data)->modified;
    if (modified) {
        gchar *text = g_strdup_printf("%s \xE2\x97\x8F", tab->title);
        gtk_label_set_text(GTK_LABEL(label), text);
        g_free(text);
    } else {
        gtk_label_set_text(GTK_LABEL(label), tab->title);
    }
}

/* focus controls whether the new page also becomes the active tab -
 * true for every explicit user action (clicking "+ Terminal", the
 * initial startup tab), false for a listener tab opening on its own
 * from a background tick (LISTENER_EVENT_STARTED can land at any time;
 * it shouldn't yank focus away from whatever the user is doing). */
/* Switches the notebook to page and grabs keyboard focus for its
 * Terminal view, if it has one - "toolbox-view" tags both terminal
 * kinds (local shell and connection-backed) identically, so this one
 * tag-based check covers both. Shared by add_tab_page's own focus
 * branch and the focus-or-open helpers below. */
void focus_page(GtkBackend *backend, GtkWidget *page) {
    gint index = gtk_notebook_page_num(GTK_NOTEBOOK(backend->notebook), page);
    if (index < 0) {
        return;
    }
    gtk_notebook_set_current_page(GTK_NOTEBOOK(backend->notebook), index);
    Terminal *view = g_object_get_data(G_OBJECT(page), "toolbox-view");
    if (view) {
        gtk_widget_grab_focus(terminal_get_widget(view));
    }
}

void add_tab_page(GtkBackend *backend, Tab *tab, gboolean focus) {
    Workspace *workspace = workbench_get_workspace(backend->workbench);

    GtkWidget *page;
    if (tab->type == TAB_TYPE_TERMINAL) {
        page = build_terminal_page(backend, tab);
    } else if (tab->type == TAB_TYPE_LISTENER) {
        page = build_listener_page(backend, tab);
    } else if (tab->type == TAB_TYPE_CONNECTION_TERMINAL) {
        page = build_connection_terminal_page(backend, tab);
    } else if (tab->type == TAB_TYPE_EDITOR) {
        page = build_editor_page(backend, tab);
    } else if (tab->type == TAB_TYPE_BINARY_INFO) {
        page = build_binary_info_page(backend, tab);
    } else {
        page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_set_border_width(GTK_CONTAINER(page), 12);
        gtk_box_pack_start(GTK_BOX(page), gtk_label_new(tab->title), FALSE, FALSE, 0);
    }

    g_object_set_data(G_OBJECT(page), "toolbox-tab", tab);
    g_object_set_data(G_OBJECT(page), "toolbox-workspace", workspace);
    g_object_set_data(G_OBJECT(page), "toolbox-backend", backend);

    GtkWidget *label = build_tab_label(tab, page);

    gtk_notebook_append_page(GTK_NOTEBOOK(backend->notebook), page, label);
    gtk_widget_show_all(page);

    if (focus) {
        focus_page(backend, page);
    }
}

void on_add_tab_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkBackend *backend = user_data;
    Workspace *workspace = workbench_get_workspace(backend->workbench);

    char title[32];
    g_snprintf(title, sizeof(title), "Terminal %d", backend->next_terminal_number++);

    Tab *tab = tab_create(TAB_TYPE_TERMINAL, title);
    workspace_add_tab(workspace, tab);
    add_tab_page(backend, tab, TRUE);
}

void on_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
    (void)notebook;
    (void)page_num;
    Workspace *workspace = user_data;
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    if (!tab) {
        return;
    }
    workspace_set_active_tab(workspace, tab->id);
    if (tab->type == TAB_TYPE_TERMINAL) {
        Terminal *view = g_object_get_data(G_OBJECT(page), "toolbox-view");
        if (view) {
            gtk_widget_grab_focus(terminal_get_widget(view));
        }
    }
}
/* --- end Generic notebook/tab lifecycle ------------------------------------ */
