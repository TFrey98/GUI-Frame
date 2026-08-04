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
 * at once). Cut/Copy (ui_gtk_menus.c) set this via
 * explorer_set_clipboard(); Paste (either trigger) reads it. `source`
 * holds an EXPLORER_SOURCE_FILES/TOOLKIT value (that enum is declared
 * below, after GtkBackend - both are plain ints, no ordering dependency). */
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
    EXPLORER_COL_PATH, /* root-relative for either source - see explorer_root_for_source() */
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
/* Opens a new terminal tab with a plain interactive shell rooted at
 * relative_directory (resolved against root) - backs "Open in
 * Integrated Terminal" (a folder's own path) and "Open in Terminal
 * Directory" (an ordinary file's parent). */
void open_terminal_at(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_directory);
/* Opens a new terminal tab and runs request as its only child (a real
 * argv spawn, never a shell string) - backs "Run in Terminal"/"Run
 * with Arguments..." when creating a new terminal. Neither this nor
 * run_command_in_active_terminal below takes ownership of
 * request->arguments/env_overrides - the caller keeps owning and
 * freeing whatever it parsed. */
void run_command_in_new_terminal(GtkBackend *backend, const TerminalLaunchRequest *request, char **env_overrides,
                                  size_t env_override_count);
/* Types request into the currently active TAB_TYPE_TERMINAL tab's
 * already-live shell (every argument/override individually
 * g_shell_quote()'d, never raw-concatenated) - backs "Run with
 * Arguments..." when reusing an existing terminal. Shows an error
 * rather than silently doing nothing if no terminal tab is active. */
void run_command_in_active_terminal(GtkBackend *backend, const TerminalLaunchRequest *request, char **env_overrides,
                                     size_t env_override_count);

/* --- ui_gtk_file_tree.c ------------------------------------------------ */
GtkWidget *build_explorer_sidebar(GtkBackend *backend);
void load_row_children(GtkBackend *backend, GtkTreeStore *store, GtkTreeIter *iter);
void refresh_row_preserving_expansion(GtkBackend *backend, GtkTreeView *tree_view, GtkTreeStore *store,
                                       GtkTreeIter *iter);
void start_new_entry(GtkBackend *backend, GtkTreeIter *parent_iter, gboolean is_folder);
/* Picks the right WorkspaceRoot for a row's EXPLORER_COL_SOURCE value -
 * every explorer action that used to hardcode
 * workbench_get_file_workspace_root() goes through this instead, so
 * the same logic works for either source unchanged. */
const WorkspaceRoot *explorer_root_for_source(GtkBackend *backend, int source);
/* FileTreeNode already caches executable/read_only (lstat at scan
 * time); ToolkitEntry has neither field. Computes both fresh via
 * stat()+access(W_OK) - identical logic to file_tree.c's own
 * classify_entry(), just not cached anywhere for Toolkit. */
void explorer_toolkit_file_flags(const char *absolute_path, bool *out_executable, bool *out_read_only);
/* Applies one FileWatchEvent (drained from backend->file_watcher/
 * toolkit_watcher, source picking which) to the explorer tree and any
 * open editor tab - called from ui_gtk_window.c's on_tick. */
void apply_file_watch_event(GtkBackend *backend, int source, const FileWatchEvent *event);
/* Finds the GtkTreeIter under scope (searched recursively, matching
 * each row's own EXPLORER_COL_PATH directly) whose root-relative path
 * equals relative_path - "" means scope itself. Only ever needs to
 * succeed as far as an already-loaded directory goes. Promoted from
 * static (Step 6) for reuse by perform_explorer_paste's source-parent
 * refresh. */
gboolean find_dir_iter_by_relative_path(GtkTreeModel *model, GtkTreeIter *scope, const char *relative_path,
                                         GtkTreeIter *out);
/* Sets backend->explorer_clipboard and enables the toolbar Paste
 * button - the one entry point both Cut and Copy (ui_gtk_menus.c) call. */
void explorer_set_clipboard(GtkBackend *backend, ExplorerClipboardMode mode, int source, const char *relative_path);
/* Pastes the clipboard's item into dest_parent_iter's directory - the
 * one entry point both the toolbar Paste button and the context menu's
 * Paste item call. A no-op (shows an error) if the clipboard is empty,
 * the paste would put a folder inside itself/its own subfolder, or the
 * underlying file_copy()/file_move() fails. CUT mode clears the
 * clipboard on success (a cut is spent after one paste). */
void perform_explorer_paste(GtkBackend *backend, GtkTreeIter *dest_parent_iter);

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
/* Triggered from the explorer's context menu (menus.c builds the item),
 * but unconditionally just shows a dialog - lives here per the same
 * splitting rule Properties/New Listener already follow. */
void open_run_with_arguments_dialog(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path,
                                     GtkWindow *parent);

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
/* The one entry point ui_gtk_file_tree.c calls for a non-directory row
 * of either source - resolves relative_path against root, classifies,
 * dedups against any already-open tab for the same (root,
 * relative_path) pair, and opens (or silently declines, via
 * show_explorer_error()) accordingly. executable/read_only come from
 * the caller's already-known bits (FileTreeNode for FILES,
 * explorer_toolkit_file_flags() for Toolkit). */
void open_or_focus_file_tab(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path,
                             bool executable, bool read_only);
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
 * file_rename() succeeds - updates any open editor/binary-info tab for
 * (root, old_relative_path) (EditorDocument, Tab title, tab label, and
 * the editor page's own header label) so a future Save keeps
 * targeting the right file. A no-op if nothing has that file open. */
void editor_handle_external_rename(GtkBackend *backend, const WorkspaceRoot *root, const char *old_relative_path,
                                    const char *new_relative_path);
/* Generalizes editor_handle_external_rename() to a possibly cross-root
 * move (Cut+Paste, drag-and-drop) - also reassigns doc->root, not just
 * relative_path/display_name/tab title. editor_handle_external_rename()
 * is now a thin same-root wrapper around this. */
void editor_handle_external_move(GtkBackend *backend, const WorkspaceRoot *old_root, const char *old_relative_path,
                                  const WorkspaceRoot *new_root, const char *new_relative_path);
/* Called from apply_file_watch_event() for a FILE_WATCH_DELETED event -
 * a no-op unless (root, relative_path) has an open editor/binary-info
 * tab, in which case it sets doc->deleted_on_disk and shows the "Deleted
 * from disk" banner on that tab. */
void editor_handle_external_delete(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path);
/* Called from apply_file_watch_event() for a FILE_WATCH_MODIFIED event -
 * a no-op unless (root, relative_path) has an open TAB_TYPE_EDITOR tab.
 * Compares a fresh stat() mtime against doc->last_known_mtime to ignore
 * an echo of this app's own save; otherwise reloads silently if the doc
 * has no unsaved changes, or sets doc->externally_modified and shows the
 * Compare/Reload from Disk/Keep Editor Version conflict dialog if it
 * does. */
void editor_handle_external_modification(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path);
/* Back Ctrl+S/Ctrl+Shift+S - resolve the notebook's current page and
 * no-op unless it's a writable TAB_TYPE_EDITOR tab. */
void trigger_save_active_editor(GtkBackend *backend);
void trigger_save_as_active_editor(GtkBackend *backend, GtkWindow *window);

#endif /* TOOLBOX_UI_GTK_INTERNAL_H */
