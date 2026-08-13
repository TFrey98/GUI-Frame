#ifndef TOOLBOX_UI_GTK_TERMINAL_INTERNAL_H
#define TOOLBOX_UI_GTK_TERMINAL_INTERNAL_H

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

/* --- ui_gtk_connection_terminal.c (connection-backed terminal view) --- */
GtkWidget *build_connection_terminal_page(GtkBackend *backend, Tab *tab);
void open_or_focus_connection_terminal(GtkBackend *backend, uint64_t connection_id);
void refresh_all_connection_terminal_pages(GtkBackend *backend);

#endif /* TOOLBOX_UI_GTK_TERMINAL_INTERNAL_H */
