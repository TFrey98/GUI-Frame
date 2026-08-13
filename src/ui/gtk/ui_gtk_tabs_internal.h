#ifndef WORKBENCH_UI_GTK_TABS_INTERNAL_H
#define WORKBENCH_UI_GTK_TABS_INTERNAL_H

/*
 * Declarations shared across the tab-lifecycle feature: generic
 * notebook/tab management (ui_gtk_tabs.c), tab label rename/close/
 * context-menu (ui_gtk_tab_labels.c), close/quit confirmation
 * (ui_gtk_tab_close.c), and the listener console tab
 * (ui_gtk_listener_page.c).
 */

#include "ui_gtk_backend.h"

/* Built by ui_gtk_listener_page.c's build_listener_page and read
 * directly by ui_gtk_tab_close.c's close-confirmation handlers - shared
 * because it's built in one file and its concrete fields are
 * dereferenced in another (same reasoning as ui_gtk_explorer_internal.h's
 * ExplorerMenuContext). */
typedef struct ListenerPageContext {
    GtkBackend *backend;
    uint64_t listener_id;
} ListenerPageContext;

/* --- ui_gtk_tabs.c (core: generic notebook/tab lifecycle) ------------- */
void add_tab_page(GtkBackend *backend, Tab *tab, gboolean focus);
void focus_page(GtkBackend *backend, GtkWidget *page);
void on_add_tab_clicked(GtkButton *button, gpointer user_data);
void on_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
/* Recomputes a page's notebook tab label from tab->title, appending a
 * trailing " ●" whenever it's a modified TAB_TYPE_EDITOR page - called
 * from ui_gtk_editor.c on every modified-state change (buffer edited,
 * saved, reverted, Save As). */
void update_tab_label_text(GtkWidget *page);
/* The actual close logic - shared by every tab type/state, unconditional
 * once called. The listener-running/modified-editor confirmation that
 * can intercept *before* this runs lives in ui_gtk_tab_close.c. */
void close_tab_page(GtkWidget *page);

/* --- ui_gtk_tab_labels.c (rename, close button, right-click menu) ----- */
/* Builds "Title  x" with the title double-click-to-rename and the x
 * closing the page - called from ui_gtk_tabs.c's add_tab_page. */
GtkWidget *build_tab_label(Tab *tab, GtkWidget *page);

/* --- ui_gtk_tab_close.c (close/quit confirmation) ---------------------- */
/* Runs pages (GtkWidget* GPtrArray, ownership taken) through the same
 * per-page Save/Discard/Cancel (modified editor) or Close Tab/Stop
 * Listener/Cancel (running listener) confirmation sequentially, calling
 * on_finished(backend, all_completed, user_data) once done (NULL is
 * fine - close_page_with_confirmation below doesn't need one). Shared
 * by the × button/Close Others/Close All (ui_gtk_tab_labels.c) and
 * prepare_window_close below. */
void run_close_operation(GtkBackend *backend, GPtrArray *pages,
                          void (*on_finished)(GtkBackend *, gboolean, gpointer), gpointer user_data);
/* Runs page through the same confirmation as run_close_operation above -
 * also backs Ctrl+Shift+W on the active page. */
void close_page_with_confirmation(GtkBackend *backend, GtkWidget *page);
/* Runs every open page through the same per-page confirmation as
 * close_page_with_confirmation, then destroys window - but only if
 * every page was actually resolved (a Cancel anywhere leaves window
 * and every tab exactly as they were). The doc's "closing the
 * application prompts for every modified document." */
void prepare_window_close(GtkBackend *backend, GtkWindow *window);

/* --- ui_gtk_listener_page.c (listener console tab) --------------------- */
/* Called from ui_gtk_tabs.c's add_tab_page for a TAB_TYPE_LISTENER tab. */
GtkWidget *build_listener_page(GtkBackend *backend, Tab *tab);
gboolean has_listener_tab(GtkBackend *backend, uint64_t listener_id);
void open_listener_tab(GtkBackend *backend, uint64_t listener_id);
void focus_or_open_listener_tab(GtkBackend *backend, uint64_t listener_id);
void refresh_all_listener_tabs(GtkBackend *backend);

#endif /* WORKBENCH_UI_GTK_TABS_INTERNAL_H */
