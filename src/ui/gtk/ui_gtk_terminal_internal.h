#ifndef WORKBENCH_UI_GTK_TERMINAL_INTERNAL_H
#define WORKBENCH_UI_GTK_TERMINAL_INTERNAL_H

/*
 * Declarations shared across the terminal feature: local-shell terminal
 * tabs (ui_gtk_terminal.c) and the connection-backed terminal view
 * (ui_gtk_connection_terminal.c).
 */

#include "ui_gtk_backend.h"

/* --- ui_gtk_terminal.c (local-shell terminal tabs) --------------------- */
GtkWidget *build_terminal_page(GtkBackend *backend, Tab *tab);
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
/* Looks up a tracked terminal by TerminalSession.id - the Objects
 * panel's own row id for a Terminal-kind row (see ui_gtk_object_list.c's
 * sync_terminal_row/refresh_object_panel). NULL if not tracked. */
TerminalEntry *find_terminal_entry_by_session_id(GtkBackend *backend, uint64_t session_id);
/* Called from ui_gtk_tabs.c's close_tab_page for TAB_TYPE_TERMINAL -
 * removes page from the notebook without destroying anything (the live
 * shell, Terminal view, TerminalSession, and Tab all stay tracked in
 * backend->terminal_entries), so accidentally closing a terminal tab
 * can never lose work. */
void undock_terminal_tab(GtkBackend *backend, GtkWidget *page);
/* Bottom-panel "Objects" tracking for local-shell terminals (see
 * undock_terminal_tab above). Docked: just focuses it. Undocked:
 * re-docks the same page into the notebook and focuses it. Popped out
 * (see pop_out_terminal_tab below): presents its standalone window
 * rather than forcing it back into the notebook - popping back in is
 * its own deliberate action. A no-op if session_id isn't tracked at all
 * (already destroyed, or never existed). */
void focus_or_reopen_terminal_tab(GtkBackend *backend, uint64_t session_id);
/* Called from the tab's own right-click menu ("Pop Out",
 * ui_gtk_tab_labels.c) for TAB_TYPE_TERMINAL - moves a currently-docked
 * terminal's page into its own standalone GtkWindow with a "Pop In"
 * button to bring it back. Terminals only ever originate docked in the
 * main workbench; this is strictly a post-creation relocation. A no-op
 * if page isn't a currently-docked terminal page. */
void pop_out_terminal_tab(GtkBackend *backend, GtkWidget *page);
/* The only path that actually ends a terminal's session - kills its
 * shell (if still running) and frees the TerminalSession/Terminal/Tab/
 * page, docked or not. Undocking via the tab's own x never does this on
 * its own; called from the Objects panel's Close menu action. */
void destroy_terminal_object(GtkBackend *backend, uint64_t session_id);

/* --- ui_gtk_connection_terminal.c (connection-backed terminal view) --- */
GtkWidget *build_connection_terminal_page(GtkBackend *backend, Tab *tab);
void open_or_focus_connection_terminal(GtkBackend *backend, uint64_t connection_id);
void refresh_all_connection_terminal_pages(GtkBackend *backend);

#endif /* WORKBENCH_UI_GTK_TERMINAL_INTERNAL_H */
