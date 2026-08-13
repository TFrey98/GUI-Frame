#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <vte/vte.h>

#include "byte_buffer.h"
#include "database.h"
#include "line_accumulator.h"
#include "pty_worker.h"
#include "terminal_history.h"
#include "terminal_vte.h"

/* --- terminal_vte.c: local-shell terminal backend -----------------------
 * The app owns the pty itself (PtyWorker: fork/exec + a poll()-driven
 * reader/writer thread, the same shape connection_worker.c uses for a
 * socket) rather than letting libvte spawn/own it - this is what makes a
 * local shell's raw input/output bytes observable for capture into the
 * database, the same way a connection's bytes already are via
 * TerminalHistory. VteTerminal is reduced to a pure display surface for
 * PTY-backed terminals: typed input is picked up via VTE's own "commit"
 * signal (the same hook the connection-backed terminal view already uses
 * for terminals with no local child at all) and written to the pty
 * ourselves; received bytes are drained from the worker once per GUI tick
 * (terminal_pump_pty_output) and fed back with terminal_feed_output(),
 * exactly like a connection terminal's retained history is replayed.
 * Output is only persisted to the database while `capturing_output` is
 * true - the window between a submitted command line and the next
 * keystroke - so a fast typist's own echoed input, however many
 * characters land in one drain, never reaches the database; only the
 * response that follows a command does. */

struct Terminal {
    VteTerminal *widget;
    TerminalSession *session; /* not owned - the owning Tab holds it */
    TerminalCommitHandler commit_handler;
    void *commit_handler_data;

    gboolean pty_active; /* true between a successful spawn and teardown/child-exit-join */
    gboolean exit_handled;
    PtyWorker pty_worker;
    TerminalHistory *history;    /* NULL until pty_active */
    LineAccumulator *input_lines; /* NULL until pty_active */
    /* True from the moment a submitted line is recorded until the next
     * keystroke - see on_commit/flush_input_line below for where this
     * flips, and terminal_pump_pty_output for where it gates the
     * database write. */
    gboolean capturing_output;
    glong last_columns;
    glong last_rows;
};

/* Fires once line_accumulator_feed (below) finds a complete submitted
 * line - i.e. right after Enter. Records the "input" row, then opens the
 * output-capture window: bytes received from here until the user's next
 * keystroke (see on_commit) are the command's actual response, so they're
 * the only output persisted to the database. */
static void flush_input_line(const char *line, size_t len, void *user_data) {
    Terminal *terminal = user_data;
    if (terminal->session) {
        database_record_terminal_event("local", terminal->session->id, "input", line, len);
    }
    terminal->capturing_output = TRUE;
}

/* Fires on VTE's own "commit" signal - the path VTE uses to report
 * "the user (or a paste, or feed_child) wants to send this onward",
 * regardless of whether VTE itself owns any child. For a PTY-backed
 * terminal, this is now the *only* path input takes (VTE no longer has
 * its own spawned child to forward to internally) - buffered into
 * complete lines for capture, then written to the pty. For a
 * display-only terminal (no local pty, e.g. a connection view), falls
 * back to the caller-registered handler, unchanged from before. */
static void on_commit(VteTerminal *vte, gchar *text, guint size, gpointer user_data) {
    (void)vte;
    Terminal *terminal = user_data;
    if (terminal->pty_active) {
        /* Typing again closes the previous command's output-capture
         * window (if flush_input_line doesn't immediately reopen it
         * below, e.g. this commit is itself the Enter that completes a
         * line) - this is what stops a fast typist's own echo from ever
         * being mistaken for output, regardless of how many characters
         * land in a single drain. */
        terminal->capturing_output = FALSE;
        line_accumulator_feed(terminal->input_lines, text, size, flush_input_line, terminal);
        pty_worker_send(&terminal->pty_worker, text, size);
        return;
    }
    if (terminal->commit_handler) {
        terminal->commit_handler(text, size, terminal->commit_handler_data);
    }
}

/* VTE recomputes its own column/row count in its (RUN_FIRST) default
 * "size-allocate" handler before any externally-connected handler like
 * this one runs, so vte_terminal_get_column_count()/get_row_count()
 * already reflect the new size by the time this fires. Only relevant
 * once a pty is actually attached - resizing an empty/display-only
 * terminal has nothing to forward. */
static void on_size_allocate(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data) {
    (void)widget;
    (void)allocation;
    Terminal *terminal = user_data;
    if (!terminal->pty_active) {
        return;
    }
    glong columns = vte_terminal_get_column_count(terminal->widget);
    glong rows = vte_terminal_get_row_count(terminal->widget);
    if (columns == terminal->last_columns && rows == terminal->last_rows) {
        return;
    }
    terminal->last_columns = columns;
    terminal->last_rows = rows;

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)columns;
    ws.ws_row = (unsigned short)rows;
    ioctl(terminal->pty_worker.master_fd, TIOCSWINSZ, &ws);
}

Terminal *terminal_create(void) {
    Terminal *terminal = malloc(sizeof(Terminal));
    terminal->widget = VTE_TERMINAL(vte_terminal_new());
    g_object_ref_sink(terminal->widget);
    terminal->session = NULL;
    terminal->commit_handler = NULL;
    terminal->commit_handler_data = NULL;
    terminal->pty_active = FALSE;
    terminal->exit_handled = FALSE;
    terminal->history = NULL;
    terminal->input_lines = NULL;
    terminal->capturing_output = FALSE;
    terminal->last_columns = 0;
    terminal->last_rows = 0;
    g_signal_connect(terminal->widget, "commit", G_CALLBACK(on_commit), terminal);
    g_signal_connect(terminal->widget, "size-allocate", G_CALLBACK(on_size_allocate), terminal);
    return terminal;
}

void terminal_destroy(Terminal *terminal) {
    if (!terminal) {
        return;
    }
    g_signal_handlers_disconnect_by_func(terminal->widget, G_CALLBACK(on_commit), terminal);
    g_signal_handlers_disconnect_by_func(terminal->widget, G_CALLBACK(on_size_allocate), terminal);
    if (terminal->pty_active) {
        pty_worker_signal_stop(&terminal->pty_worker);
        pty_worker_join(&terminal->pty_worker);
        terminal->pty_active = FALSE;
    }
    if (terminal->history) {
        terminal_history_destroy(terminal->history);
    }
    if (terminal->input_lines) {
        line_accumulator_destroy(terminal->input_lines);
    }
    g_object_unref(terminal->widget);
    free(terminal);
}

int terminal_start_shell(Terminal *terminal, TerminalSession *session) {
    terminal->session = session;

    char *argv[] = {session->shell_path, NULL};
    const char *working_directory =
        (session->working_directory && *session->working_directory) ? session->working_directory : NULL;

    if (pty_worker_start(&terminal->pty_worker, session->shell_path, argv, NULL, working_directory) != 0) {
        g_printerr("workbench: could not start a shell for session '%s'\n", session->title);
        return -1;
    }

    terminal->pty_active = TRUE;
    terminal->exit_handled = FALSE;
    terminal->history = terminal_history_create("local", session->id);
    terminal->input_lines = line_accumulator_create();
    terminal_session_mark_running(session);
    return 0;
}

int terminal_run_command(Terminal *terminal, TerminalSession *session, const TerminalLaunchRequest *request,
                          char **env_overrides, size_t env_override_count) {
    terminal->session = session;

    size_t argc = 1 + request->argument_count;
    char **argv = malloc(sizeof(char *) * (argc + 1));
    argv[0] = (char *)request->executable;
    for (size_t i = 0; i < request->argument_count; i++) {
        argv[1 + i] = request->arguments[i];
    }
    argv[argc] = NULL;

    const char *working_directory = request->working_directory[0] ? request->working_directory : NULL;

    /* NULL (inherit unchanged) unless there's actually an override to
     * apply - g_get_environ() + g_environ_setenv() per override builds
     * a real merged envv rather than hand-assembling one. */
    gchar **envv = NULL;
    for (size_t i = 0; i < env_override_count; i++) {
        char *eq = strchr(env_overrides[i], '=');
        if (!eq) {
            continue;
        }
        size_t key_len = (size_t)(eq - env_overrides[i]);
        char key[256];
        if (key_len >= sizeof(key)) {
            continue;
        }
        memcpy(key, env_overrides[i], key_len);
        key[key_len] = '\0';
        if (!envv) {
            envv = g_get_environ();
        }
        envv = g_environ_setenv(envv, key, eq + 1, TRUE);
    }

    /* fork() (inside pty_worker_start) COW-copies this process's memory,
     * so the child's own view of argv/envv is unaffected by freeing them
     * here in the parent right afterward - same reasoning that already
     * applied when this function's spawn call was async. */
    int rc = pty_worker_start(&terminal->pty_worker, request->executable, argv, envv, working_directory);
    free(argv);
    if (envv) {
        g_strfreev(envv);
    }

    if (rc != 0) {
        g_printerr("workbench: could not run command for session '%s'\n", session->title);
        return -1;
    }

    terminal->pty_active = TRUE;
    terminal->exit_handled = FALSE;
    terminal->history = terminal_history_create("local", session->id);
    terminal->input_lines = line_accumulator_create();
    terminal_session_mark_running(session);

    /* This command was never "typed" through commit/terminal_send - it's
     * spawned directly as argv (the file explorer's Run in Terminal/Run
     * with Arguments actions, when "Create new terminal" is checked) - so
     * without this, nothing would record it as input or open the output-
     * capture window for whatever it prints. Rebuild the same shell-quoted
     * line run_command_in_active_terminal() would have sent a live
     * terminal, and feed it through the exact same capture path a real
     * submitted line takes. */
    GString *command_line = g_string_new(NULL);
    for (size_t i = 0; i < env_override_count; i++) {
        char *eq = strchr(env_overrides[i], '=');
        if (!eq) {
            continue;
        }
        char *key = g_strndup(env_overrides[i], (gsize)(eq - env_overrides[i]));
        char *quoted_value = g_shell_quote(eq + 1);
        g_string_append_printf(command_line, "%s=%s ", key, quoted_value);
        g_free(key);
        g_free(quoted_value);
    }
    char *quoted_exe = g_shell_quote(request->executable);
    g_string_append(command_line, quoted_exe);
    g_free(quoted_exe);
    for (size_t i = 0; i < request->argument_count; i++) {
        char *quoted_arg = g_shell_quote(request->arguments[i]);
        g_string_append_printf(command_line, " %s", quoted_arg);
        g_free(quoted_arg);
    }
    flush_input_line(command_line->str, command_line->len, terminal);
    g_string_free(command_line, TRUE);

    return 0;
}

int terminal_send(Terminal *terminal, const char *data, size_t length) {
    if (!terminal || !data || !terminal->pty_active) {
        return -1;
    }
    /* Same capture treatment as real typed input (on_commit) - a
     * programmatically injected command (e.g. "Run in active terminal")
     * is still terminal input from the user's perspective, just typed on
     * their behalf, and closes any still-open output-capture window from
     * a previous command the same way a real keystroke would. */
    terminal->capturing_output = FALSE;
    line_accumulator_feed(terminal->input_lines, data, length, flush_input_line, terminal);
    pty_worker_send(&terminal->pty_worker, data, length);
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

void terminal_pump_pty_output(Terminal *terminal) {
    if (!terminal || !terminal->pty_active) {
        return;
    }

    unsigned char buf[4096];
    size_t n;
    while ((n = byte_buffer_consume(terminal->pty_worker.incoming, buf, sizeof(buf))) > 0) {
        terminal_history_append(terminal->history, buf, n, terminal->capturing_output);
        terminal_feed_output(terminal, (const char *)buf, n);
    }

    if (!terminal->exit_handled && atomic_load(terminal->pty_worker.exited)) {
        int status = atomic_load(terminal->pty_worker.exit_status);
        if (terminal->session) {
            terminal_session_mark_exited(terminal->session, status);
        }
        static const char message[] = "\r\n[Process exited]\r\n";
        vte_terminal_feed(terminal->widget, message, (gssize)strlen(message));
        terminal->exit_handled = TRUE;

        /* The thread has already returned by the time *exited is
         * observed true, so this join completes immediately - it's only
         * here to reclaim the worker's buffers/atomics/wake pipe. */
        pty_worker_join(&terminal->pty_worker);
        terminal->pty_active = FALSE;
    }
}

GtkWidget *terminal_get_widget(Terminal *terminal) {
    return GTK_WIDGET(terminal->widget);
}

/* VSCode's default "Dark+" integrated-terminal palette (foreground,
 * background, and the 16 ANSI colors) - kept as parsed strings rather
 * than magic GdkRGBA literals so the mapping to VSCode's own theme
 * stays obvious at a glance. */
void terminal_apply_theme(Terminal *terminal, gboolean dark) {
    if (!dark) {
        vte_terminal_set_colors(terminal->widget, NULL, NULL, NULL, 0);
        return;
    }

    GdkRGBA fg, bg;
    gdk_rgba_parse(&fg, "#d4d4d4");
    gdk_rgba_parse(&bg, "#1e1e1e");

    static const char *const ansi_hex[16] = {
        "#000000", "#cd3131", "#0dbc79", "#e5e510", "#2472c8", "#bc3fbc", "#11a8cd", "#e5e5e5",
        "#666666", "#f14c4c", "#23d18b", "#f5f543", "#3b8eea", "#d670d6", "#29b8db", "#e5e5e5",
    };
    GdkRGBA palette[16];
    for (int i = 0; i < 16; i++) {
        gdk_rgba_parse(&palette[i], ansi_hex[i]);
    }

    vte_terminal_set_colors(terminal->widget, &fg, &bg, palette, 16);
}
