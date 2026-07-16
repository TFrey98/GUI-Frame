#include <gtk/gtk.h>
#include <stdlib.h>

#include "../../core/tab.h"
#include "../../core/terminal_session.h"
#include "../../core/workspace.h"
#include "../../ui/workbench.h"
#include "terminal_vte.h"

/* Tracks a live terminal tab's View+Session pair independent of the GTK
 * widget tree. The notebook and its pages are owned by the window and are
 * gone by the time platform_ui_destroy runs (the window closes and GTK
 * tears down its children during the run loop, well before app_destroy
 * gets to call us) - backend->notebook would be a dangling pointer at
 * that point, so shutdown cleanup walks this array instead. */
typedef struct TerminalEntry {
    Terminal *view;
    TerminalSession *session; /* not owned - the Tab owns it */
} TerminalEntry;

typedef struct GtkBackend {
    Workbench *workbench;
    GtkApplication *gtk_app;
    GtkWidget *notebook;
    int next_terminal_number;
    GPtrArray *terminal_entries; /* TerminalEntry* */
} GtkBackend;

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

typedef struct TabLabelData {
    Tab *tab;
    GtkWidget *label;
    GtkWidget *entry;
} TabLabelData;

static void on_toggle_panel(GtkToggleButton *button, gpointer user_data) {
    GtkWidget *panel = GTK_WIDGET(user_data);
    if (gtk_toggle_button_get_active(button)) {
        gtk_widget_show(panel);
    } else {
        gtk_widget_hide(panel);
    }
}

static void commit_rename(GtkEntry *entry, gpointer user_data) {
    TabLabelData *data = user_data;
    const char *text = gtk_entry_get_text(entry);
    if (text && *text) {
        tab_set_title(data->tab, text);
        gtk_label_set_text(GTK_LABEL(data->label), text);
        if (data->tab->type == TAB_TYPE_TERMINAL && data->tab->backend_data) {
            terminal_session_set_title((TerminalSession *)data->tab->backend_data, text);
        }
    }
    gtk_widget_hide(data->entry);
    gtk_widget_show(data->label);
}

static gboolean on_entry_focus_out(GtkWidget *entry, GdkEventFocus *event, gpointer user_data) {
    (void)event;
    commit_rename(GTK_ENTRY(entry), user_data);
    return FALSE;
}

static gboolean on_label_button_press(GtkWidget *event_box, GdkEventButton *event, gpointer user_data) {
    (void)event_box;
    TabLabelData *data = user_data;
    if (event->type == GDK_2BUTTON_PRESS && event->button == 1) {
        gtk_entry_set_text(GTK_ENTRY(data->entry), data->tab->title);
        gtk_widget_hide(data->label);
        gtk_widget_show(data->entry);
        gtk_widget_grab_focus(data->entry);
        gtk_editable_select_region(GTK_EDITABLE(data->entry), 0, -1);
        return TRUE;
    }
    return FALSE;
}

static void on_tab_close_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWidget *page = GTK_WIDGET(user_data);
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    Workspace *workspace = g_object_get_data(G_OBJECT(page), "toolbox-workspace");
    GtkWidget *notebook = gtk_widget_get_ancestor(page, GTK_TYPE_NOTEBOOK);

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
    }

    workspace_close_tab(workspace, tab_id);
    if (page_num >= 0) {
        gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), page_num);
    }
}

/* Builds "Title  x" with the title double-click-to-rename and the x
 * closing the page. `page` must already carry "toolbox-tab"/
 * "toolbox-workspace" object data. */
static GtkWidget *build_tab_label(Tab *tab, GtkWidget *page) {
    GtkWidget *label = gtk_label_new(tab->title);

    GtkWidget *entry = gtk_entry_new();
    gtk_widget_set_no_show_all(entry, TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 10);

    GtkWidget *label_stack = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(label_stack), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(label_stack), entry, FALSE, FALSE, 0);

    GtkWidget *event_box = gtk_event_box_new();
    gtk_widget_add_events(event_box, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(event_box), label_stack);

    TabLabelData *data = g_new0(TabLabelData, 1);
    data->tab = tab;
    data->label = label;
    data->entry = entry;
    g_object_set_data_full(G_OBJECT(event_box), "toolbox-label-data", data, g_free);

    g_signal_connect(event_box, "button-press-event", G_CALLBACK(on_label_button_press), data);
    g_signal_connect(entry, "activate", G_CALLBACK(commit_rename), data);
    g_signal_connect(entry, "focus-out-event", G_CALLBACK(on_entry_focus_out), data);

    GtkWidget *close_button = gtk_button_new_with_label("×");
    gtk_button_set_relief(GTK_BUTTON(close_button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close_button, FALSE);
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_tab_close_clicked), page);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(box), event_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), close_button, FALSE, FALSE, 0);
    gtk_widget_show_all(box);

    return box;
}

/* The Tab owns the TerminalSession (session identity - id, title, cwd,
 * shell path, running/exit state - independent of any widget); the page
 * widget carries the View (the live VteTerminal wrapper) as its own
 * object data, since the View is purely a platform-layer concern. */
static GtkWidget *build_terminal_page(GtkBackend *backend, Tab *tab) {
    TerminalSession *session = terminal_session_create(tab->id, tab->title);
    tab->backend_data = session;

    Terminal *view = terminal_create();
    if (terminal_start_shell(view, session) != 0) {
        g_printerr("toolbox: could not start a shell for tab '%s'\n", tab->title);
    }

    TerminalEntry *entry = g_new(TerminalEntry, 1);
    entry->view = view;
    entry->session = session;
    g_ptr_array_add(backend->terminal_entries, entry);

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroller), terminal_get_widget(view));
    g_object_set_data(G_OBJECT(scroller), "toolbox-view", view);
    g_object_set_data(G_OBJECT(scroller), "toolbox-backend", backend);
    return scroller;
}

static void add_tab_page(GtkBackend *backend, Tab *tab) {
    Workspace *workspace = workbench_get_workspace(backend->workbench);

    GtkWidget *page;
    if (tab->type == TAB_TYPE_TERMINAL) {
        page = build_terminal_page(backend, tab);
    } else {
        page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_set_border_width(GTK_CONTAINER(page), 12);
        gtk_box_pack_start(GTK_BOX(page), gtk_label_new(tab->title), FALSE, FALSE, 0);
    }

    g_object_set_data(G_OBJECT(page), "toolbox-tab", tab);
    g_object_set_data(G_OBJECT(page), "toolbox-workspace", workspace);

    GtkWidget *label = build_tab_label(tab, page);

    gint index = gtk_notebook_append_page(GTK_NOTEBOOK(backend->notebook), page, label);
    gtk_widget_show_all(page);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(backend->notebook), index);

    if (tab->type == TAB_TYPE_TERMINAL) {
        Terminal *view = g_object_get_data(G_OBJECT(page), "toolbox-view");
        if (view) {
            gtk_widget_grab_focus(terminal_get_widget(view));
        }
    }
}

static void on_add_tab_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkBackend *backend = user_data;
    Workspace *workspace = workbench_get_workspace(backend->workbench);

    char title[32];
    g_snprintf(title, sizeof(title), "Terminal %d", backend->next_terminal_number++);

    Tab *tab = tab_create(TAB_TYPE_TERMINAL, title);
    workspace_add_tab(workspace, tab);
    add_tab_page(backend, tab);
}

static void on_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
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

static GtkWidget *build_sidebar(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(box, 160, -1);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("Sidebar"), FALSE, FALSE, 0);
    return box;
}

static GtkWidget *build_bottom_panel(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(box, -1, 120);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("Bottom Panel"), FALSE, FALSE, 0);
    return box;
}

static GtkWidget *build_top_bar(GtkWidget *sidebar, GtkWidget *bottom_panel) {
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 6);

    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("toolbox"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new(NULL), TRUE, TRUE, 0);

    GtkWidget *sidebar_toggle = gtk_toggle_button_new_with_label("Sidebar");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sidebar_toggle), TRUE);
    g_signal_connect(sidebar_toggle, "toggled", G_CALLBACK(on_toggle_panel), sidebar);
    gtk_box_pack_start(GTK_BOX(bar), sidebar_toggle, FALSE, FALSE, 0);

    GtkWidget *bottom_toggle = gtk_toggle_button_new_with_label("Bottom Panel");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bottom_toggle), TRUE);
    g_signal_connect(bottom_toggle, "toggled", G_CALLBACK(on_toggle_panel), bottom_panel);
    gtk_box_pack_start(GTK_BOX(bar), bottom_toggle, FALSE, FALSE, 0);

    return bar;
}

static GtkWidget *build_workbench_layout(GtkBackend *backend) {
    Workspace *workspace = workbench_get_workspace(backend->workbench);

    GtkWidget *sidebar = build_sidebar();
    GtkWidget *bottom_panel = build_bottom_panel();
    GtkWidget *top_bar = build_top_bar(sidebar, bottom_panel);

    backend->notebook = gtk_notebook_new();
    g_signal_connect(backend->notebook, "switch-page", G_CALLBACK(on_notebook_switch_page), workspace);

    GtkWidget *add_button = gtk_button_new_with_label("+ Terminal");
    gtk_button_set_relief(GTK_BUTTON(add_button), GTK_RELIEF_NONE);
    g_signal_connect(add_button, "clicked", G_CALLBACK(on_add_tab_clicked), backend);
    gtk_widget_show(add_button);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(backend->notebook), add_button, GTK_PACK_END);

    backend->next_terminal_number = 1;
    Tab *first_tab = tab_create(TAB_TYPE_TERMINAL, "Terminal 1");
    workspace_add_tab(workspace, first_tab);
    add_tab_page(backend, first_tab);
    backend->next_terminal_number = 2;

    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(hpaned), sidebar, FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(hpaned), backend->notebook, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(hpaned), 220);

    GtkWidget *vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(vpaned), hpaned, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(vpaned), bottom_panel, FALSE, FALSE);
    gtk_paned_set_position(GTK_PANED(vpaned), 480);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(root), top_bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), vpaned, TRUE, TRUE, 0);

    return root;
}

static void on_activate(GtkApplication *gtk_app, gpointer user_data) {
    GtkBackend *backend = user_data;

    GtkWidget *window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(window), "toolbox");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 700);
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);

    gtk_container_add(GTK_CONTAINER(window), build_workbench_layout(backend));

    gtk_widget_show_all(window);
}

void *platform_ui_create(Workbench *workbench) {
    GtkBackend *backend = malloc(sizeof(GtkBackend));
    backend->workbench = workbench;
    backend->notebook = NULL;
    backend->next_terminal_number = 1;
    backend->terminal_entries = g_ptr_array_new();
    backend->gtk_app = gtk_application_new("dev.toolbox.app", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(backend->gtk_app, "activate", G_CALLBACK(on_activate), backend);
    return backend;
}

int platform_ui_run(void *backend_ptr, int argc, char **argv) {
    GtkBackend *backend = backend_ptr;
    return g_application_run(G_APPLICATION(backend->gtk_app), argc, argv);
}

void platform_ui_destroy(void *backend_ptr) {
    GtkBackend *backend = backend_ptr;
    if (!backend) {
        return;
    }

    /* Individually-closed tabs already removed and freed their entry in
     * on_tab_close_clicked; this covers whatever's left when the whole
     * window closes without every tab being closed first. The View holds
     * its own widget reference (see terminal_create's g_object_ref_sink),
     * so it's still safe to destroy here even though the window - and
     * therefore backend->notebook - is long gone by this point. */
    for (guint i = 0; i < backend->terminal_entries->len; i++) {
        TerminalEntry *entry = g_ptr_array_index(backend->terminal_entries, i);
        terminal_destroy(entry->view);
        terminal_session_destroy(entry->session);
        g_free(entry);
    }
    g_ptr_array_free(backend->terminal_entries, TRUE);

    g_object_unref(backend->gtk_app);
    free(backend);
}
