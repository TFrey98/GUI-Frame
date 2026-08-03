#include "ui_gtk_internal.h"

/* Approximates VSCode's default "Dark+" color theme using plain GTK3 CSS
 * node selectors (window, button, treeview, textview, ...) rather than
 * custom style classes, so it applies uniformly to every widget this app
 * already builds without touching a single widget-construction site. */
static const char *const DARK_THEME_CSS =
    "window { background-color: #1e1e1e; color: #cccccc; }\n"
    "label { color: #cccccc; }\n"
    "button { background-image: none; background-color: #3c3c3c; color: #cccccc; border-color: #3c3c3c; }\n"
    "button:hover { background-color: #45494e; }\n"
    "button:checked, button:active { background-color: #094771; color: #ffffff; }\n"
    "entry { background-color: #3c3c3c; color: #cccccc; border-color: #3c3c3c; caret-color: #cccccc; }\n"
    "notebook { background-color: #1e1e1e; }\n"
    "notebook > header { background-color: #252526; border-color: #252526; }\n"
    "notebook > header tab { background-color: #2d2d2d; color: #969696; border-color: #252526; }\n"
    "notebook > header tab:checked { background-color: #1e1e1e; color: #ffffff; }\n"
    "notebook > header tab button { background-color: transparent; border-color: transparent; color: #969696; }\n"
    "treeview { background-color: #252526; color: #cccccc; }\n"
    "treeview:selected, treeview row:selected { background-color: #094771; color: #ffffff; }\n"
    "treeview header button { background-color: #252526; color: #cccccc; border-color: #1e1e1e; }\n"
    "textview, textview text { background-color: #1e1e1e; color: #d4d4d4; caret-color: #ffffff; }\n"
    "textview text:selected { background-color: #264f78; color: #ffffff; }\n"
    "scrollbar { background-color: #1e1e1e; }\n"
    "scrollbar slider { background-color: #4d4d4d; }\n"
    "scrollbar slider:hover { background-color: #5a5a5a; }\n"
    "paned > separator { background-color: #2d2d2d; }\n"
    "separator { background-color: #2d2d2d; }\n"
    "menu { background-color: #252526; color: #cccccc; border-color: #454545; }\n"
    "menuitem:hover { background-color: #094771; color: #ffffff; }\n"
    "dialog, .background { background-color: #252526; color: #cccccc; }\n"
    "tooltip { background-color: #252526; color: #cccccc; }\n";

void gtk_theme_init(GtkBackend *backend) {
    backend->css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(backend->css_provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* Mirrors refresh_all_connection_terminal_pages' "walk every notebook
 * page" pattern - "toolbox-view" is tagged identically by
 * build_terminal_page and build_connection_terminal_page, so this one
 * loop reaches every open terminal regardless of kind. */
static void apply_theme_to_open_terminals(GtkBackend *backend) {
    if (!backend->notebook) {
        return;
    }
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(backend->notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), i);
        Terminal *view = g_object_get_data(G_OBJECT(page), "toolbox-view");
        if (view) {
            terminal_apply_theme(view, backend->dark_mode);
        }
    }
}

void apply_dark_mode(GtkBackend *backend, gboolean dark) {
    backend->dark_mode = dark;
    gtk_css_provider_load_from_data(backend->css_provider, dark ? DARK_THEME_CSS : "", -1, NULL);
    apply_theme_to_open_terminals(backend);
}
