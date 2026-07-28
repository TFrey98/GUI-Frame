#ifndef TOOLBOX_TERMINAL_H
#define TOOLBOX_TERMINAL_H

#include <stddef.h>

#include "../core/terminal_session.h"

typedef struct Terminal Terminal;

Terminal *terminal_create(void);
void terminal_destroy(Terminal *terminal);

/* Spawns session->shell_path inside session->working_directory, updating
 * session->running/exit_code as the child starts and later exits - the
 * session stays an accurate record even though it never touches the
 * widget itself. Returns 0 on success. */
int terminal_start_shell(Terminal *terminal, TerminalSession *session);

/* Feeds data to the terminal's child process, as if it had been typed. */
int terminal_send(Terminal *terminal, const char *data, size_t length);

/* Feeds data to the terminal's display only - never routed to a child
 * process. For terminals with no spawned shell (e.g. rendering a live
 * connection's retained history + incoming bytes). */
void terminal_feed_output(Terminal *terminal, const char *data, size_t length);

/* Fires with whatever the terminal wants to send onward - the same path
 * a PTY-backed terminal uses internally to reach its child, which fires
 * regardless of whether a child is actually attached. For a terminal
 * with no spawned shell, this is the only way to observe what the user
 * typed. NULL clears the handler. */
typedef void (*TerminalCommitHandler)(const char *data, size_t length, void *user_data);
void terminal_set_commit_handler(Terminal *terminal, TerminalCommitHandler handler, void *user_data);

#endif /* TOOLBOX_TERMINAL_H */
