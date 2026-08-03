#ifndef TOOLBOX_UI_GTK_INTERNAL_H
#define TOOLBOX_UI_GTK_INTERNAL_H

/*
 * Shared package-private header for every .c file under src/ui/gtk/.
 * This directory is tightly coupled by design - the split across files
 * is for readability, not independent reuse - so one shared header
 * (mirroring a single translation unit split across files) is simpler
 * than a maze of per-file headers. Contains: GtkBackend and every other
 * struct/enum more than one file needs, plus a declaration for every
 * function now called across one of the file boundaries below. Anything
 * not listed here stays `static` and file-local, exactly as it was
 * before this split.
 */

#include <gtk/gtk.h>
#include <stdint.h>

#include "core/tab.h"
#include "core/terminal_session.h"
#include "core/workspace.h"
#include "files/editor_document.h"
#include "files/file_classify.h"
#include "files/file_operations.h"
#include "files/file_tree.h"
#include "listeners/listener_system.h"
#include "listeners/object_predicates.h"
#include "tools/toolkit_index.h"
#include "ui/workbench.h"
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
    int next_terminal_number; /* feeds the default "Terminal N" title; only
                                * ever increments, so closed numbers are not
                                * reused within a run. */
    GPtrArray *terminal_entries; /* TerminalEntry* */

    ListenerSystem *listener_system; /* borrowed from workbench */
    uint64_t last_listener_id;       /* most recently created listener; 0 = none yet */
    GtkWidget *status_label;         /* shows last_listener_id's name + state, via on_tick */
    int next_listener_number;        /* feeds the default "Listener N" name, mirrors
                                       * next_terminal_number above */
    GtkTreeStore *object_panel_store; /* bottom panel: listeners -> connections, see refresh_object_panel */
    guint tick_source_id;
    FileTree *file_tree;             /* backs the "TOOLBOX" root in the merged explorer sidebar */
    GtkTreeStore *explorer_store;    /* merged explorer sidebar: TOOLBOX (files/) + Toolkit (toolkit/) */
    GtkWidget *explorer_tree_view;
    GtkCellRenderer *explorer_name_renderer;
    GtkTreeRowReference *explorer_editing_row; /* the row currently in inline create/rename, if any */

    GtkCssProvider *css_provider; /* app-wide dark/light stylesheet, see ui_gtk_theme.c */
    gboolean dark_mode;           /* current toggle state; new terminals/pages read this to match */
} GtkBackend;

/* Bottom object panel tree columns - object_list.c builds/syncs this
 * store; menus.c's object context menu reads a row's id from it. */
enum {
    OBJECT_PANEL_COL_NAME,
    OBJECT_PANEL_COL_ENDPOINT,
    OBJECT_PANEL_COL_STATE,
    OBJECT_PANEL_COL_ID,
    OBJECT_PANEL_COL_COUNT
};

/* Explorer sidebar tree columns/sources - file_tree.c builds/syncs this
 * store; menus.c's explorer context menu (and dialogs.c's Properties
 * dialog) read these same columns. */
enum {
    EXPLORER_COL_ICON,
    EXPLORER_COL_NAME,
    EXPLORER_COL_PATH, /* FILES rows: relative_path; TOOLKIT rows: absolute path */
    EXPLORER_COL_IS_DIR,
    EXPLORER_COL_LOADED,
    EXPLORER_COL_SOURCE,
    EXPLORER_COL_NODE_ID, /* FileNodeId - FILES rows only, unused for TOOLKIT rows */
    EXPLORER_COL_COUNT
};

enum {
    EXPLORER_SOURCE_FILES,
    EXPLORER_SOURCE_TOOLKIT
};

/* GTK-layer-only sentinel (never a real FileNodeId - those start at 1
 * and only ever increase) marking a blank row being named for a
 * pending New File/New Folder, so the shared "edited" handler can tell
 * a create from a rename. */
#define EXPLORER_PENDING_CREATE_ID ((guint64)0xFFFFFFFFFFFFFFFFULL)

/* Built by menus.c's popup_explorer_context_menu and read by its own
 * item handlers *and* dialogs.c's on_explorer_menu_properties - the one
 * cross-file case, since Properties otherwise lives in dialogs.c (see
 * the restructuring plan's splitting rule). Safe to trust directly
 * (unlike the object panel's file-local MenuItemContext, which
 * re-checks live state): nothing but user actions ever mutates this
 * tree, and a popup menu grabs input so nothing else can run between
 * menu-open and item-click. */
typedef struct ExplorerMenuContext {
    GtkBackend *backend;
    GtkTreeIter iter; /* the row the menu was built for */
} ExplorerMenuContext;

/* --- ui_gtk_tabs.c --------------------------------------------------- */
void add_tab_page(GtkBackend *backend, Tab *tab, gboolean focus);
void focus_page(GtkBackend *backend, GtkWidget *page);
gboolean has_listener_tab(GtkBackend *backend, uint64_t listener_id);
void open_listener_tab(GtkBackend *backend, uint64_t listener_id);
void focus_or_open_listener_tab(GtkBackend *backend, uint64_t listener_id);
void refresh_all_listener_tabs(GtkBackend *backend);
void on_add_tab_clicked(GtkButton *button, gpointer user_data);
void on_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
/* Recomputes a page's notebook tab label from tab->title, appending a
 * trailing " ●" whenever it's a modified TAB_TYPE_EDITOR page - called
 * from ui_gtk_editor.c on every modified-state change (buffer edited,
 * saved, reverted, Save As). */
void update_tab_label_text(GtkWidget *page);
/* Runs page through the same Save/Discard/Cancel (modified editor) or
 * Close Tab/Stop Listener/Cancel (running listener) confirmation
 * on_tab_close_clicked already used for the × button - also backs
 * Ctrl+Shift+W on the active page. */
void close_page_with_confirmation(GtkBackend *backend, GtkWidget *page);
/* Runs every open page through the same per-page confirmation as
 * close_page_with_confirmation, then destroys window - but only if
 * every page was actually resolved (a Cancel anywhere leaves window
 * and every tab exactly as they were). The doc's "closing the
 * application prompts for every modified document." */
void prepare_window_close(GtkBackend *backend, GtkWindow *window);

/* --- ui_gtk_terminal.c ------------------------------------------------ */
GtkWidget *build_terminal_page(GtkBackend *backend, Tab *tab);
GtkWidget *build_connection_terminal_page(GtkBackend *backend, Tab *tab);
void open_or_focus_connection_terminal(GtkBackend *backend, uint64_t connection_id);
void refresh_all_connection_terminal_pages(GtkBackend *backend);

/* --- ui_gtk_file_tree.c ------------------------------------------------ */
GtkWidget *build_explorer_sidebar(GtkBackend *backend);
void load_row_children(GtkBackend *backend, GtkTreeStore *store, GtkTreeIter *iter);
void refresh_row_preserving_expansion(GtkBackend *backend, GtkTreeView *tree_view, GtkTreeStore *store,
                                       GtkTreeIter *iter);
void start_new_entry(GtkBackend *backend, GtkTreeIter *parent_iter, gboolean is_folder);

/* --- ui_gtk_object_list.c ---------------------------------------------- */
GtkWidget *build_bottom_panel(GtkBackend *backend);
void refresh_object_panel(GtkBackend *backend);
/* Shared string formatters - object_list.c defines them (its sync
 * functions are the primary consumer), tabs.c's listener console tab
 * also uses them. */
const char *listener_state_name(ListenerState state);
const char *listener_type_label(ListenerType type);
const char *connection_state_name(ConnectionState state);

/* --- ui_gtk_menus.c ----------------------------------------------------- */
void popup_object_context_menu(GtkBackend *backend, GtkWidget *tree_view, GtkTreePath *path, GdkEventButton *event);
gboolean on_object_panel_button_press(GtkWidget *tree_view, GdkEventButton *event, gpointer user_data);
gboolean on_object_panel_popup_menu(GtkWidget *tree_view, gpointer user_data);
void popup_explorer_context_menu(GtkBackend *backend, GtkWidget *tree_view, GtkTreePath *path,
                                  GdkEventButton *event);
gboolean on_explorer_button_press(GtkWidget *tree_view, GdkEventButton *event, gpointer user_data);
gboolean on_explorer_popup_menu(GtkWidget *tree_view, gpointer user_data);

/* --- ui_gtk_dialogs.c ---------------------------------------------------- */
GtkWidget *add_form_row(GtkGrid *grid, int row, const char *label_text, GtkWidget *entry);
GtkWidget *add_error_row(GtkGrid *grid, int row);
void show_explorer_error(GtkBackend *backend, const char *message);
const char *file_operation_error_message(FileOperationResult result);
void on_new_listener_clicked(GtkButton *button, gpointer user_data);
/* Properties is triggered from the explorer's context menu (menus.c
 * builds the menu item), but its entire effect is unconditionally
 * showing a dialog, so its definition lives in dialogs.c - see the
 * restructuring plan's splitting rule. */
void on_explorer_menu_properties(GtkMenuItem *item, gpointer user_data);
const char *editor_save_error_message(EditorSaveResult result);

/* --- ui_gtk_window.c ----------------------------------------------------- */
void on_activate(GtkApplication *gtk_app, gpointer user_data);

/* --- ui_gtk_theme.c ------------------------------------------------------ */
/* Attaches backend->css_provider to the default screen at application
 * priority - must run once, before the first widget is realized (called
 * from on_activate). Starts inert (dark_mode is FALSE at this point), so
 * this alone doesn't change anything visible yet. */
void gtk_theme_init(GtkBackend *backend);
/* Toggled by the "Dark Mode" button in the top bar - swaps the app-wide
 * CSS stylesheet and re-applies VTE colors (see terminal_apply_theme) to
 * every currently open terminal page. */
void apply_dark_mode(GtkBackend *backend, gboolean dark);

/* --- ui_gtk_editor.c ----------------------------------------------------- */
GtkWidget *build_editor_page(GtkBackend *backend, Tab *tab);
GtkWidget *build_binary_info_page(GtkBackend *backend, Tab *tab);
/* The one entry point ui_gtk_file_tree.c calls for a non-directory
 * FILES row - resolves, classifies, dedups against any already-open
 * tab for the same relative_path, and opens (or silently declines,
 * via show_explorer_error()) accordingly. */
void open_or_focus_file_tab(GtkBackend *backend, const FileTreeNode *node);
/* Extracts page's live buffer text and saves it in place via
 * editor_document_save(); on success refreshes the page's Save/Revert
 * sensitivity + tab label. Shows no dialog itself either way - callers
 * that trigger a single save (the Save button, Ctrl+S, the close/quit
 * confirmation flows) show one via editor_save_error_message() on
 * failure; save_all_modified_editors batches failures into one summary
 * dialog instead of one per file. The one save primitive every other
 * save/close/quit path reuses. */
EditorSaveResult save_editor_page(GtkBackend *backend, GtkWidget *page);
/* Saves every writable+modified TAB_TYPE_EDITOR page; reports every
 * failure (not just the first) in one summary dialog - the doc's
 * "Save All reports each failure rather than silently skipping files." */
void save_all_modified_editors(GtkBackend *backend);
/* Called from ui_gtk_file_tree.c right after a real (non-create)
 * file_rename() succeeds - updates any open editor/binary-info tab
 * for old_relative_path (EditorDocument, Tab title, tab label, and
 * the editor page's own header label) so a future Save keeps
 * targeting the right file. A no-op if nothing has that file open. */
void editor_handle_external_rename(GtkBackend *backend, const char *old_relative_path,
                                    const char *new_relative_path);
/* Back Ctrl+S/Ctrl+Shift+S - resolve the notebook's current page and
 * no-op unless it's a writable TAB_TYPE_EDITOR tab. */
void trigger_save_active_editor(GtkBackend *backend);
void trigger_save_as_active_editor(GtkBackend *backend, GtkWindow *window);

#endif /* TOOLBOX_UI_GTK_INTERNAL_H */
