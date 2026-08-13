#include "ui_gtk_backend.h"
#include "ui_gtk_tabs_internal.h"

/* --- Tab label: rename, close button, context menu ------------------------
 * The tab label widget's own interactive behavior - double-click to
 * rename, the × close button, and the right-click Close/Close Others/
 * Close All menu. Generic tab lifecycle (add_tab_page, close_tab_page)
 * lives in ui_gtk_tabs.c; the confirmation workflow the close actions
 * below route through lives in ui_gtk_tab_close.c. */

/* Shared by a tab label's click and entry-commit handlers so both can flip
 * between the display label and the editable rename entry for the same
 * tab. Owned by the event_box in build_tab_label via g_object_set_data_full,
 * so it's freed automatically when that widget is destroyed. */
typedef struct TabLabelData {
    Tab *tab;
    GtkWidget *label;
    GtkWidget *entry;
    GtkWidget *page; /* back-reference, needed by the right-click context menu below */
} TabLabelData;

/* Forward declared for on_label_button_press below - defined further
 * down, after the close/quit confirmation section (it needs
 * run_close_operation). */
static void popup_tab_context_menu(GtkWidget *page, GdkEventButton *event);
static gboolean on_tab_label_popup_menu(GtkWidget *event_box, gpointer user_data);

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
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
        popup_tab_context_menu(data->page, event);
        return TRUE;
    }
    return FALSE;
}

/* Bound to the × button on every tab label - derives backend from the
 * page itself (add_tab_page tags "workbench-backend" on every page it
 * builds) since the button's own "clicked" signal only ever carries
 * the page as user_data. */
static void on_tab_close_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWidget *page = GTK_WIDGET(user_data);
    GtkBackend *backend = g_object_get_data(G_OBJECT(page), "workbench-backend");
    close_page_with_confirmation(backend, page);
}

/* Builds "Title  x" with the title double-click-to-rename and the x
 * closing the page. `page` must already carry "workbench-tab"/
 * "workbench-workspace" object data. */
GtkWidget *build_tab_label(Tab *tab, GtkWidget *page) {
    GtkWidget *label = gtk_label_new(tab->title);
    g_object_set_data(G_OBJECT(page), "workbench-tab-label-widget", label);

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
    data->page = page;
    g_object_set_data_full(G_OBJECT(event_box), "workbench-label-data", data, g_free);

    g_signal_connect(event_box, "button-press-event", G_CALLBACK(on_label_button_press), data);
    g_signal_connect(event_box, "popup-menu", G_CALLBACK(on_tab_label_popup_menu), data);
    g_signal_connect(entry, "activate", G_CALLBACK(commit_rename), data);
    g_signal_connect(entry, "focus-out-event", G_CALLBACK(on_entry_focus_out), data);

    GtkWidget *close_button = gtk_button_new_with_label("×");
    gtk_button_set_relief(GTK_BUTTON(close_button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close_button, FALSE);
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_tab_close_clicked), page);
    g_object_set_data(G_OBJECT(page), "workbench-tab-close-button", close_button);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(box), event_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), close_button, FALSE, FALSE, 0);
    gtk_widget_show_all(box);

    return box;
}

/* --- Tab label right-click menu: Close / Close Others / Close All ----- */

static void on_tab_context_close(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *page = user_data;
    GtkBackend *backend = g_object_get_data(G_OBJECT(page), "workbench-backend");
    close_page_with_confirmation(backend, page);
}

static void on_tab_context_close_others(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *page = user_data;
    GtkBackend *backend = g_object_get_data(G_OBJECT(page), "workbench-backend");
    GPtrArray *pages = g_ptr_array_new();
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *candidate = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i);
        if (candidate != page) {
            g_ptr_array_add(pages, candidate);
        }
    }
    run_close_operation(backend, pages, NULL, NULL);
}

static void on_tab_context_close_all(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *page = user_data;
    GtkBackend *backend = g_object_get_data(G_OBJECT(page), "workbench-backend");
    GPtrArray *pages = g_ptr_array_new();
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        g_ptr_array_add(pages, gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i));
    }
    run_close_operation(backend, pages, NULL, NULL);
}

/* event NULL means the keyboard "popup-menu" signal (see
 * on_tab_label_popup_menu below) - gtk_menu_popup_at_pointer() already
 * accepts NULL there, same convention popup_object_context_menu/
 * popup_explorer_context_menu already established. Tags the built menu
 * on page itself so it stays discoverable after this call returns -
 * tests read the menu this way, a real user never needs to. */
static void popup_tab_context_menu(GtkWidget *page, GdkEventButton *event) {
    GtkWidget *menu = gtk_menu_new();

    GtkWidget *close_item = gtk_menu_item_new_with_label("Close");
    GtkWidget *close_others_item = gtk_menu_item_new_with_label("Close Others");
    GtkWidget *close_all_item = gtk_menu_item_new_with_label("Close All");
    g_signal_connect(close_item, "activate", G_CALLBACK(on_tab_context_close), page);
    g_signal_connect(close_others_item, "activate", G_CALLBACK(on_tab_context_close_others), page);
    g_signal_connect(close_all_item, "activate", G_CALLBACK(on_tab_context_close_all), page);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), close_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), close_others_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), close_all_item);

    g_object_set_data(G_OBJECT(page), "workbench-tab-context-menu", menu);
    gtk_widget_show_all(menu);
    if (event) {
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    } else {
        gtk_menu_popup_at_widget(GTK_MENU(menu), page, GDK_GRAVITY_CENTER, GDK_GRAVITY_CENTER, NULL);
    }
}

/* Shift+F10/Menu-key equivalent of the right-click handler above - same
 * "real, standard GTK signal" convention this suite already relies on
 * for the object panel's and explorer's own context menus. */
static gboolean on_tab_label_popup_menu(GtkWidget *event_box, gpointer user_data) {
    (void)event_box;
    TabLabelData *data = user_data;
    popup_tab_context_menu(data->page, NULL);
    return TRUE;
}
/* --- end tab label right-click menu -------------------------------------- */
/* --- end Tab label: rename, close button, context menu -------------------- */
