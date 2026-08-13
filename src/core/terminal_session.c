#include "terminal_session.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Monotonic counter, not thread-safe - fine as long as sessions are only
 * ever created from the single GTK main-loop thread. */
static uint64_t g_next_session_id = 1;

static char *detect_shell_path(void) {
    const char *shell = getenv("SHELL");
    if (!shell || !*shell) {
        shell = "/bin/bash";
    }
    return strdup(shell);
}

/* New terminals should default to the user's home directory, like every
 * other terminal emulator - not wherever the app binary happened to be
 * launched from. */
static char *detect_working_directory(void) {
    const char *home = getenv("HOME");
    if (home && *home) {
        return strdup(home);
    }
    char *cwd = getcwd(NULL, 0);
    return cwd ? cwd : strdup("");
}

TerminalSession *terminal_session_create(uint64_t tab_id, const char *title) {
    TerminalSession *session = malloc(sizeof(TerminalSession));
    session->id = g_next_session_id++;
    session->tab_id = tab_id;
    session->title = strdup(title ? title : "Untitled");
    session->working_directory = detect_working_directory();
    session->shell_path = detect_shell_path();
    session->running = 0;
    session->exit_code = 0;
    return session;
}

void terminal_session_destroy(TerminalSession *session) {
    if (!session) {
        return;
    }
    free(session->title);
    free(session->working_directory);
    free(session->shell_path);
    free(session);
}

void terminal_session_set_title(TerminalSession *session, const char *title) {
    free(session->title);
    session->title = strdup(title);
}

void terminal_session_set_working_directory(TerminalSession *session, const char *working_directory) {
    free(session->working_directory);
    session->working_directory = strdup(working_directory);
}

void terminal_session_mark_running(TerminalSession *session) {
    session->running = 1;
}

void terminal_session_mark_exited(TerminalSession *session, int exit_code) {
    session->running = 0;
    session->exit_code = exit_code;
}
