#include "ui_gtk_backend.h"
#include "ui_gtk_tabs_internal.h"
#include "ui_gtk_terminal_internal.h"

#include <stdio.h>
#include <string.h>

/* --- ui_gtk_terminal.c: local-shell terminal tabs ----------------------
 * A TAB_TYPE_TERMINAL page always wraps a real spawned shell (or a
 * one-shot command). The connection-backed terminal view (no PTY, no
 * local shell) lives in ui_gtk_connection_terminal.c instead. */

/* Lets a caller pre-configure a terminal tab's session (a custom
 * working directory, or a real command to run instead of an
 * interactive shell) before add_tab_page/build_terminal_page create
 * its page. This is the same "pre-seed tab->backend_data, let the
 * builder consume it" pattern editor tabs use. NULL launch_request means
 * "just start a plain shell in session's own working directory" (backs Open in
 * Integrated Terminal/Open in Terminal Directory); non-NULL means
 * "run this command instead" (backs Run in Terminal/Run with
 * Arguments). Consumed and freed by build_terminal_page - never seen
 * outside this file. */
typedef struct PendingTerminalSpawn {
    TerminalSession *session;              /* pre-configured, owned - becomes tab->backend_data below */
    TerminalLaunchRequest *launch_request;  /* NULL = plain interactive shell */
    char **env_overrides;                   /* NULL-terminated "KEY=VALUE" array, NULL = none */
} PendingTerminalSpawn;

static void pending_terminal_spawn_destroy(PendingTerminalSpawn *pending) {
    if (!pending) {
        return;
    }
    if (pending->launch_request) {
        g_strfreev(pending->launch_request->arguments);
        free(pending->launch_request);
    }
    g_strfreev(pending->env_overrides);
    free(pending);
}

/* The Tab owns the TerminalSession (session identity - id, title, cwd,
 * shell path, running/exit state - independent of any widget); the page
 * widget carries the View (the live VteTerminal wrapper) as its own
 * object data, since the View is purely a platform-layer concern. */
GtkWidget *build_terminal_page(GtkBackend *backend, Tab *tab) {
    PendingTerminalSpawn *pending = tab->backend_data;
    TerminalSession *session;
    Terminal *view = terminal_create();
    terminal_apply_theme(view, backend->dark_mode);

    if (!pending) {
        session = terminal_session_create(tab->id, tab->title);
        if (terminal_start_shell(view, session) != 0) {
            g_printerr("toolbox: could not start a shell for tab '%s'\n", tab->title);
        }
    } else {
        session = pending->session;
        if (pending->launch_request) {
            size_t env_count = pending->env_overrides ? g_strv_length(pending->env_overrides) : 0;
            if (terminal_run_command(view, session, pending->launch_request, pending->env_overrides, env_count) !=
                0) {
                g_printerr("toolbox: could not run command for tab '%s'\n", tab->title);
            }
        } else if (terminal_start_shell(view, session) != 0) {
            g_printerr("toolbox: could not start a shell for tab '%s'\n", tab->title);
        }
        pending_terminal_spawn_destroy(pending);
    }
    tab->backend_data = session;

    TerminalEntry *entry = g_new(TerminalEntry, 1);
    entry->view = view;
    entry->session = session;
    g_ptr_array_add(backend->terminal_entries, entry);

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroller), terminal_get_widget(view));
    g_object_set_data(G_OBJECT(scroller), "toolbox-view", view);
    g_object_set_data(G_OBJECT(scroller), "toolbox-backend", backend);
    return scroller;
}

/* --- Explorer-triggered terminal actions ------------------------------- */

static char **dup_strv(char *const *src, size_t count) {
    if (count == 0) {
        return NULL;
    }
    char **copy = g_new0(char *, count + 1);
    for (size_t i = 0; i < count; i++) {
        copy[i] = g_strdup(src[i]);
    }
    return copy;
}

void open_terminal_at(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_directory) {
    char resolved[4096];
    /* An empty relative_directory means the workspace root itself (e.g.
     * a file with no parent besides TOOLBOX) - workspace_root_resolve_path()
     * rejects an empty string outright, same special case file_tree.c's
     * own root handling already established. */
    if (!relative_directory || relative_directory[0] == '\0') {
        g_strlcpy(resolved, root->canonical_path, sizeof(resolved));
    } else if (!workspace_root_resolve_path(root, relative_directory, resolved, sizeof(resolved))) {
        show_explorer_error(backend, "Can't open a terminal there - the target is missing or outside the "
                                      "workspace.");
        return;
    }

    Workspace *workspace = workbench_get_workspace(backend->workbench);
    char title[64];
    g_snprintf(title, sizeof(title), "Terminal %d", backend->next_terminal_number++);

    Tab *tab = tab_create(TAB_TYPE_TERMINAL, title);

    TerminalSession *session = terminal_session_create(tab->id, title);
    terminal_session_set_working_directory(session, resolved);

    PendingTerminalSpawn *pending = g_new0(PendingTerminalSpawn, 1);
    pending->session = session;
    tab->backend_data = pending;

    workspace_add_tab(workspace, tab);
    add_tab_page(backend, tab, TRUE);
}

void run_command_in_new_terminal(GtkBackend *backend, const TerminalLaunchRequest *request, char **env_overrides,
                                  size_t env_override_count) {
    Workspace *workspace = workbench_get_workspace(backend->workbench);
    char title[64];
    g_snprintf(title, sizeof(title), "Terminal %d", backend->next_terminal_number++);

    Tab *tab = tab_create(TAB_TYPE_TERMINAL, title);

    TerminalSession *session = terminal_session_create(tab->id, title);
    terminal_session_set_working_directory(session, request->working_directory);

    /* Deep-copies arguments/env_overrides rather than borrowing the
     * caller's own g_shell_parse_argv()/entry-parsed arrays - the
     * caller keeps owning and freeing whatever it built, regardless of
     * which of this function or run_command_in_active_terminal below
     * actually got called (both just read, neither takes ownership). */
    TerminalLaunchRequest *request_copy = g_new(TerminalLaunchRequest, 1);
    *request_copy = *request;
    request_copy->arguments = dup_strv(request->arguments, request->argument_count);

    PendingTerminalSpawn *pending = g_new0(PendingTerminalSpawn, 1);
    pending->session = session;
    pending->launch_request = request_copy;
    pending->env_overrides = dup_strv(env_overrides, env_override_count);
    tab->backend_data = pending;

    workspace_add_tab(workspace, tab);
    add_tab_page(backend, tab, TRUE);
}

static GtkWidget *active_terminal_page(GtkBackend *backend) {
    if (!backend->notebook) {
        return NULL;
    }
    gint idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(backend->notebook));
    if (idx < 0) {
        return NULL;
    }
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(backend->notebook), idx);
    Tab *tab = g_object_get_data(G_OBJECT(page), "toolbox-tab");
    return (tab && tab->type == TAB_TYPE_TERMINAL) ? page : NULL;
}

/* vte_terminal_spawn_async can only ever attach one child per widget
 * lifetime, and every open TAB_TYPE_TERMINAL tab already has a live
 * interactive shell - so "reuse an existing terminal" can only mean
 * typing the command into it, the same way a person would. Never
 * built through raw concatenation: every argument (and every
 * "KEY=VALUE" override, using the shell's own single-command-scope
 * "VAR=val cmd" syntax) is escaped with g_shell_quote() before being
 * joined. */
void run_command_in_active_terminal(GtkBackend *backend, const TerminalLaunchRequest *request, char **env_overrides,
                                     size_t env_override_count) {
    GtkWidget *page = active_terminal_page(backend);
    Terminal *view = page ? g_object_get_data(G_OBJECT(page), "toolbox-view") : NULL;
    if (!view) {
        show_explorer_error(backend, "No terminal tab is active to run this command in.");
        return;
    }

    GString *line = g_string_new(NULL);
    for (size_t i = 0; i < env_override_count; i++) {
        char *eq = strchr(env_overrides[i], '=');
        if (!eq) {
            continue;
        }
        char *key = g_strndup(env_overrides[i], (gsize)(eq - env_overrides[i]));
        char *quoted_value = g_shell_quote(eq + 1);
        g_string_append_printf(line, "%s=%s ", key, quoted_value);
        g_free(key);
        g_free(quoted_value);
    }
    char *quoted_exe = g_shell_quote(request->executable);
    g_string_append(line, quoted_exe);
    g_free(quoted_exe);
    for (size_t i = 0; i < request->argument_count; i++) {
        char *quoted_arg = g_shell_quote(request->arguments[i]);
        g_string_append_printf(line, " %s", quoted_arg);
        g_free(quoted_arg);
    }
    g_string_append(line, "\n");

    terminal_send(view, line->str, line->len);
    g_string_free(line, TRUE);
}
