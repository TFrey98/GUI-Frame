#ifndef TOOLBOX_TERMINAL_H
#define TOOLBOX_TERMINAL_H

#include <stdbool.h>
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

typedef struct TerminalLaunchRequest {
    char working_directory[4096];
    char executable[4096];

    char **arguments;
    size_t argument_count;

    bool create_new_terminal;
} TerminalLaunchRequest;

/* Spawns request->executable + request->arguments as a real argv array
 * (never a shell string) inside request->working_directory, updating
 * session->running/exit_code the same way terminal_start_shell does.
 * env_overrides is a "KEY=VALUE" string per entry - NULL/0 means
 * inherit the parent environment unchanged, otherwise a merged envv is
 * built (the parent's own environment plus these overrides applied on
 * top). Returns 0 on success. */
int terminal_run_command(Terminal *terminal, TerminalSession *session, const TerminalLaunchRequest *request,
                          char **env_overrides, size_t env_override_count);

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
