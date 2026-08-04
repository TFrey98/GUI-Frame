#ifndef TOOLBOX_TERMINAL_SESSION_H
#define TOOLBOX_TERMINAL_SESSION_H

#include <stdint.h>

/*
 * Session identity for a terminal, independent of any GTK/VTE widget.
 * A TerminalSession can exist, be inspected, and be reasoned about without
 * a live view ever being created - the piece later needed for logging,
 * SQLite recording, session restoration, and non-Linux ports.
 */
typedef struct TerminalSession {
    uint64_t id;
    uint64_t tab_id;

    char *title;
    char *working_directory;
    char *shell_path;

    int running;
    int exit_code;
} TerminalSession;

/* working_directory defaults to $HOME (falling back to the current process
 * directory) and shell_path is resolved from $SHELL (falling back to
 * /bin/bash) - both portable, neither requiring a live terminal view. */
TerminalSession *terminal_session_create(uint64_t tab_id, const char *title);
void terminal_session_destroy(TerminalSession *session);

void terminal_session_set_title(TerminalSession *session, const char *title);
/* Overrides the default $HOME working directory - must be called
 * before terminal_start_shell()/terminal_run_command() actually spawns
 * anything. */
void terminal_session_set_working_directory(TerminalSession *session, const char *working_directory);
void terminal_session_mark_running(TerminalSession *session);
void terminal_session_mark_exited(TerminalSession *session, int exit_code);

#endif /* TOOLBOX_TERMINAL_SESSION_H */
