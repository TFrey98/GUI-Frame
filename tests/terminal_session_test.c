/*
 * Proves Milestone 6's definition of done directly: session identity
 * exists independently of the GTK widget. This test never calls gtk_init,
 * never creates a window, and never touches VTE - a TerminalSession is
 * fully usable on its own.
 */
#include <stdio.h>
#include <string.h>

#include "core/terminal_session.h"

int main(void) {
    TerminalSession *a = terminal_session_create(42, "Terminal 1");
    TerminalSession *b = terminal_session_create(42, "Terminal 2");

    if (a->id == 0 || b->id == 0 || a->id == b->id) {
        fprintf(stderr, "terminal_session_test: expected distinct nonzero ids\n");
        return 1;
    }
    if (a->tab_id != 42 || b->tab_id != 42) {
        fprintf(stderr, "terminal_session_test: tab_id not preserved\n");
        return 1;
    }
    if (strcmp(a->title, "Terminal 1") != 0) {
        fprintf(stderr, "terminal_session_test: title not preserved\n");
        return 1;
    }
    if (!a->shell_path || !*a->shell_path) {
        fprintf(stderr, "terminal_session_test: shell_path was not resolved\n");
        return 1;
    }
    if (!a->working_directory || !*a->working_directory) {
        fprintf(stderr, "terminal_session_test: working_directory was not resolved\n");
        return 1;
    }
    if (a->running != 0 || a->exit_code != 0) {
        fprintf(stderr, "terminal_session_test: a new session should start not-running\n");
        return 1;
    }

    terminal_session_mark_running(a);
    if (!a->running) {
        fprintf(stderr, "terminal_session_test: mark_running did not set running\n");
        return 1;
    }

    terminal_session_mark_exited(a, 7);
    if (a->running || a->exit_code != 7) {
        fprintf(stderr, "terminal_session_test: mark_exited did not update state\n");
        return 1;
    }

    terminal_session_set_title(a, "Renamed");
    if (strcmp(a->title, "Renamed") != 0) {
        fprintf(stderr, "terminal_session_test: set_title did not update the title\n");
        return 1;
    }

    terminal_session_destroy(a);
    terminal_session_destroy(b);

    printf("terminal_session_test: session identity verified independent of any GTK widget\n");
    return 0;
}
