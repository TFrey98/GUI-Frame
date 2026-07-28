#include <stdlib.h>
#include <string.h>
#include <vte/vte.h>

#include "terminal_vte.h"

struct Terminal {
    VteTerminal *widget;
    TerminalSession *session; /* not owned - the owning Tab holds it */
    TerminalCommitHandler commit_handler;
    void *commit_handler_data;
};

/* Fires when the spawned shell exits (e.g. the user ran `exit`). Updates
 * the session so it stays an accurate record, then feeds the notice
 * straight into the terminal's own display buffer - the tab stays open
 * and legible without the rest of the app needing to know anything
 * changed. */
static void on_child_exited(VteTerminal *vte, gint status, gpointer user_data) {
    Terminal *terminal = user_data;
    if (terminal->session) {
        terminal_session_mark_exited(terminal->session, status);
    }
    static const char message[] = "\r\n[Process exited]\r\n";
    vte_terminal_feed(vte, message, (gssize)strlen(message));
}

/* Fires on VTE's own "commit" signal - the path a PTY-backed terminal
 * uses internally to reach its child, which fires whether or not a
 * child is actually attached. Forwards to the caller's own handler, if
 * one is registered - a no-op for every terminal that never calls
 * terminal_set_commit_handler() (every local PTY-backed terminal today). */
static void on_commit(VteTerminal *vte, gchar *text, guint size, gpointer user_data) {
    (void)vte;
    Terminal *terminal = user_data;
    if (terminal->commit_handler) {
        terminal->commit_handler(text, size, terminal->commit_handler_data);
    }
}

Terminal *terminal_create(void) {
    Terminal *terminal = malloc(sizeof(Terminal));
    terminal->widget = VTE_TERMINAL(vte_terminal_new());
    g_object_ref_sink(terminal->widget);
    terminal->session = NULL;
    terminal->commit_handler = NULL;
    terminal->commit_handler_data = NULL;
    g_signal_connect(terminal->widget, "child-exited", G_CALLBACK(on_child_exited), terminal);
    g_signal_connect(terminal->widget, "commit", G_CALLBACK(on_commit), terminal);
    return terminal;
}

void terminal_destroy(Terminal *terminal) {
    if (!terminal) {
        return;
    }
    /* VTE can synthesize a "child-exited" emission while tearing down a
     * widget whose child is still running (e.g. when the containing page
     * is destroyed out from under a live shell). Disconnect first so that
     * emission can never fire into this soon-to-be-freed struct. */
    g_signal_handlers_disconnect_by_func(terminal->widget, G_CALLBACK(on_child_exited), terminal);
    g_signal_handlers_disconnect_by_func(terminal->widget, G_CALLBACK(on_commit), terminal);
    g_object_unref(terminal->widget);
    free(terminal);
}

typedef struct SpawnResult {
    GMainLoop *loop;
    gboolean success;
} SpawnResult;

static void on_spawn_complete(VteTerminal *vte, GPid pid, GError *error, gpointer user_data) {
    (void)vte;
    (void)pid;
    SpawnResult *result = user_data;
    if (error) {
        g_printerr("toolbox: failed to spawn shell: %s\n", error->message);
        result->success = FALSE;
    } else {
        result->success = TRUE;
    }
    g_main_loop_quit(result->loop);
}

int terminal_start_shell(Terminal *terminal, TerminalSession *session) {
    terminal->session = session;

    char *argv[] = {session->shell_path, NULL};
    const char *working_directory =
        (session->working_directory && *session->working_directory) ? session->working_directory : NULL;

    /* vte_terminal_spawn_async is the only spawn entry point in this VTE
     * version; a nested main loop turns it back into the synchronous call
     * this function's signature promises. */
    SpawnResult result = {
        .loop = g_main_loop_new(NULL, FALSE),
        .success = FALSE,
    };

    vte_terminal_spawn_async(
        terminal->widget,
        VTE_PTY_DEFAULT,
        working_directory,
        argv,
        NULL, /* envv: inherit */
        G_SPAWN_DEFAULT,
        NULL, NULL, NULL,
        -1, /* no timeout */
        NULL,
        on_spawn_complete,
        &result
    );

    g_main_loop_run(result.loop);
    g_main_loop_unref(result.loop);

    if (result.success) {
        terminal_session_mark_running(session);
    }
    return result.success ? 0 : -1;
}

int terminal_send(Terminal *terminal, const char *data, size_t length) {
    if (!terminal || !data) {
        return -1;
    }
    vte_terminal_feed_child(terminal->widget, data, (gssize)length);
    return 0;
}

void terminal_feed_output(Terminal *terminal, const char *data, size_t length) {
    if (!terminal || !data) {
        return;
    }
    vte_terminal_feed(terminal->widget, data, (gssize)length);
}

void terminal_set_commit_handler(Terminal *terminal, TerminalCommitHandler handler, void *user_data) {
    terminal->commit_handler = handler;
    terminal->commit_handler_data = handler ? user_data : NULL;
}

GtkWidget *terminal_get_widget(Terminal *terminal) {
    return GTK_WIDGET(terminal->widget);
}
