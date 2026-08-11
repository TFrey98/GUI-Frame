#ifndef TOOLBOX_UI_GTK_BACKEND_H
#define TOOLBOX_UI_GTK_BACKEND_H

/*
 * The one header every .c file under src/ui/gtk/ includes: GtkBackend
 * itself, the struct/enum types embedded in it or referenced across
 * more than one feature area, and declarations for functions that
 * don't belong to a single feature (app shell, theme, search, the
 * bottom object panel, and the small dialogs with no dedicated feature
 * header of their own). Feature-specific declarations instead live in
 * ui_gtk_tabs_internal.h / ui_gtk_explorer_internal.h /
 * ui_gtk_editor_internal.h / ui_gtk_terminal_internal.h - each of those
 * also includes this header, so a .c file only needs to add whichever
 * of those four it actually calls into.
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
#include "files/file_watch_event.h"
#include "files/file_watcher.h"
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

/* Single-item explorer clipboard - matches this app's own single-
 * selection tree (nothing anywhere lets a user select more than one row
 * at once). Cut/Copy (ui_gtk_explorer_menu.c) set this via
 * explorer_set_clipboard(); Paste (either trigger) reads it. `source`
 * holds an EXPLORER_SOURCE_FILES/TOOLKIT value (declared below, both
 * are plain ints, no ordering dependency). Embedded by value in
 * GtkBackend below, so it must be fully defined before it. */
typedef enum ExplorerClipboardMode {
    EXPLORER_CLIPBOARD_NONE,
    EXPLORER_CLIPBOARD_COPY,
    EXPLORER_CLIPBOARD_CUT
} ExplorerClipboardMode;

typedef struct ExplorerClipboard {
    ExplorerClipboardMode mode;
    int source; /* EXPLORER_SOURCE_FILES/TOOLKIT - which root relative_path is against */
    char relative_path[4096];
} ExplorerClipboard;

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
    ExplorerClipboard explorer_clipboard;      /* see ExplorerClipboard's own comment */
    GtkWidget *explorer_paste_button;          /* toolbar Paste button; sensitivity mirrors the clipboard's mode */

    /* The row currently being drag-and-dropped, if any - stashed in
     * on_explorer_drag_begin, read back in on_explorer_drag_data_received.
     * Source and dest are always this same tree view in this same
     * process (GTK_TARGET_SAME_APP), so this is the real source of
     * truth rather than anything serialized through GtkSelectionData. */
    gboolean explorer_drag_active;
    int explorer_drag_source;
    char explorer_drag_relative_path[4096];

    FileWatcher *file_watcher;     /* watches directories loaded under the TOOLBOX (files/) root */
    FileWatcher *toolkit_watcher;  /* watches directories loaded under the Toolkit (toolkit/) root */

    GtkCssProvider *css_provider; /* app-wide dark/light stylesheet, see ui_gtk_theme.c */
    gboolean dark_mode;           /* current toggle state; new terminals/pages read this to match */

    GtkWidget *search_window; /* singleton Search window, NULL when not open - see ui_gtk_search.c */
} GtkBackend;

/* Bottom object panel tree columns - ui_gtk_object_list.c builds/syncs
 * this store; ui_gtk_object_menu.c's context menu reads a row's id
 * from it. */
enum {
    OBJECT_PANEL_COL_NAME,
    OBJECT_PANEL_COL_ENDPOINT,
    OBJECT_PANEL_COL_STATE,
    OBJECT_PANEL_COL_ID,
    OBJECT_PANEL_COL_COUNT
};

/* Which WorkspaceRoot a row/path belongs to - used well beyond the
 * explorer widget itself (search results, the file watcher's two
 * watchers, platform_ui_create's initial clipboard state), so it lives
 * here rather than ui_gtk_explorer_internal.h. */
enum {
    EXPLORER_SOURCE_FILES,
    EXPLORER_SOURCE_TOOLKIT
};

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

/* --- ui_gtk_search.c ------------------------------------------------------- */
/* Presents backend->search_window if already open, otherwise builds and
 * shows it - never opens a second one. Bound to the top bar's Search
 * button. */
void open_or_present_search_window(GtkBackend *backend);

/* --- ui_gtk_dialogs.c (shared error-message/grid-row helpers) --------- */
GtkWidget *add_form_row(GtkGrid *grid, int row, const char *label_text, GtkWidget *entry);
GtkWidget *add_error_row(GtkGrid *grid, int row);
void show_explorer_error(GtkBackend *backend, const char *message);
const char *file_operation_error_message(FileOperationResult result);
const char *editor_save_error_message(EditorSaveResult result);

/* --- ui_gtk_listener_dialog.c (New Listener dialog) --------------------- */
void on_new_listener_clicked(GtkButton *button, gpointer user_data);

/* --- ui_gtk_run_dialog.c (Run with Arguments dialog) -------------------- */
/* Triggered from the explorer's context menu, but unconditionally just
 * shows a dialog - lives in its own file per the restructuring plan's
 * splitting rule, declared here since it has no dedicated feature
 * header of its own (needs WorkspaceRoot, already visible via
 * ui/workbench.h above). */
void open_run_with_arguments_dialog(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path,
                                     GtkWindow *parent);

/* --- ui_gtk_object_list.c ---------------------------------------------- */
GtkWidget *build_bottom_panel(GtkBackend *backend);
void refresh_object_panel(GtkBackend *backend);
/* Shared string formatters - object_list.c defines them (its sync
 * functions are the primary consumer); ui_gtk_listener_page.c and
 * ui_gtk_connection_terminal.c also use them. */
const char *listener_state_name(ListenerState state);
const char *listener_type_label(ListenerType type);
const char *connection_state_name(ConnectionState state);

/* --- ui_gtk_object_menu.c ------------------------------------------------ */
void popup_object_context_menu(GtkBackend *backend, GtkWidget *tree_view, GtkTreePath *path, GdkEventButton *event);
gboolean on_object_panel_button_press(GtkWidget *tree_view, GdkEventButton *event, gpointer user_data);
gboolean on_object_panel_popup_menu(GtkWidget *tree_view, gpointer user_data);

#endif /* TOOLBOX_UI_GTK_BACKEND_H */
