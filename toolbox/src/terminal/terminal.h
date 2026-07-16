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

#endif /* TOOLBOX_TERMINAL_H */
