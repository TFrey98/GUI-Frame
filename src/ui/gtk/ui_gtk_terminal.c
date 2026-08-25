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
            g_printerr("workbench: could not start a shell for tab '%s'\n", tab->title);
        }
    } else {
        session = pending->session;
        if (pending->launch_request) {
            size_t env_count = pending->env_overrides ? g_strv_length(pending->env_overrides) : 0;
            if (terminal_run_command(view, session, pending->launch_request, pending->env_overrides, env_count) !=
                0) {
                g_printerr("workbench: could not run command for tab '%s'\n", tab->title);
            }
        } else if (terminal_start_shell(view, session) != 0) {
            g_printerr("workbench: could not start a shell for tab '%s'\n", tab->title);
        }
        pending_terminal_spawn_destroy(pending);
    }
    tab->backend_data = session;

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroller), terminal_get_widget(view));
    g_object_set_data(G_OBJECT(scroller), "workbench-view", view);
    g_object_set_data(G_OBJECT(scroller), "workbench-backend", backend);

    TerminalEntry *entry = g_new(TerminalEntry, 1);
    entry->view = view;
    entry->session = session;
    entry->page = scroller;
    entry->dock_state = TERMINAL_DOCKED; /* add_tab_page docks it immediately after this returns */
    entry->popout_window = NULL;
    g_ptr_array_add(backend->terminal_entries, entry);

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

    /* Opt-in bottom-panel tab: a sibling "<executable>.manifest.json" is
     * only ever found (and only ever resolves its data_file) for a
     * script that actually lives under the toolkit root - see
     * tool_panel_manifest_load's own containment check - so this is a
     * silent no-op for every other kind of "Run in Terminal" launch. */
    ToolPanelManifest manifest;
    const WorkspaceRoot *toolkit_root = workbench_get_toolkit_workspace_root(backend->workbench);
    if (tool_panel_manifest_load(toolkit_root, request->executable, &manifest)) {
        open_tool_panel_tab_for_launch(backend, tab->id, &manifest);
    }
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
    Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
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
    Terminal *view = page ? g_object_get_data(G_OBJECT(page), "workbench-view") : NULL;
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

/* --- Terminal object lifecycle (undock-not-destroy, Objects panel) ----- */

TerminalEntry *find_terminal_entry_by_session_id(GtkBackend *backend, uint64_t session_id) {
    for (guint i = 0; i < backend->terminal_entries->len; i++) {
        TerminalEntry *entry = g_ptr_array_index(backend->terminal_entries, i);
        if (entry->session->id == session_id) {
            return entry;
        }
    }
    return NULL;
}

static TerminalEntry *find_terminal_entry_by_page(GtkBackend *backend, GtkWidget *page) {
    for (guint i = 0; i < backend->terminal_entries->len; i++) {
        TerminalEntry *entry = g_ptr_array_index(backend->terminal_entries, i);
        if (entry->page == page) {
            return entry;
        }
    }
    return NULL;
}

/* Drops entry's own bookkeeping (never its page/view/session - the
 * caller decides those separately, either keeping them alive for a
 * later re-dock or destroying them outright). Matches by view pointer,
 * same as the pre-undo-able-close version of this function used to. */
static void remove_terminal_entry(GtkBackend *backend, Terminal *view) {
    for (guint i = 0; i < backend->terminal_entries->len; i++) {
        TerminalEntry *entry = g_ptr_array_index(backend->terminal_entries, i);
        if (entry->view == view) {
            g_ptr_array_remove_index_fast(backend->terminal_entries, i);
            g_free(entry);
            return;
        }
    }
}

/* Called from ui_gtk_tabs.c's close_tab_page for TAB_TYPE_TERMINAL - a
 * terminal tab's x undocks rather than destroys (see close_tab_page's
 * own comment). A no-op if page isn't a currently-tracked terminal page
 * (shouldn't happen, but matches close_tab_page's existing page_num
 * >= 0 guard style rather than assuming). */
void undock_terminal_tab(GtkBackend *backend, GtkWidget *page) {
    int page_num = gtk_notebook_page_num(GTK_NOTEBOOK(backend->notebook), page);
    if (page_num < 0) {
        return;
    }
    TerminalEntry *entry = find_terminal_entry_by_page(backend, page);
    if (entry) {
        entry->dock_state = TERMINAL_UNDOCKED;
    }
    g_object_ref(page); /* survive the notebook's own remove-page teardown */
    gtk_notebook_remove_page(GTK_NOTEBOOK(backend->notebook), page_num);
}

/* Shared re-dock tail for TERMINAL_UNDOCKED and TERMINAL_POPPED_OUT -
 * both converge here holding exactly one ref on entry->page with no
 * container owning it, then hand that ref to the notebook. The old tab
 * label (docked case) or popout toolbar (popped-out case) is gone by
 * now, so a fresh label is built the same way add_tab_page does. */
static void redock_terminal_entry(GtkBackend *backend, TerminalEntry *entry) {
    if (entry->dock_state == TERMINAL_POPPED_OUT) {
        GtkWidget *popout_window = entry->popout_window;
        GtkWidget *container = gtk_widget_get_parent(entry->page);
        g_object_ref(entry->page); /* survive removal until the notebook re-adds it below */
        gtk_container_remove(GTK_CONTAINER(container), entry->page);
        entry->popout_window = NULL;
        gtk_widget_destroy(popout_window); /* now just the empty toolbar/box - page already extracted */
    }
    /* else TERMINAL_UNDOCKED: already holds this same kind of extra ref
     * (taken when it was undocked), so both branches now converge on
     * "page is ownerless, held only by our one extra ref" before the
     * shared docking below, which hands that ref to the notebook. */

    Tab *tab = g_object_get_data(G_OBJECT(entry->page), "workbench-tab");
    GtkWidget *label = build_tab_label(tab, entry->page);
    gtk_notebook_append_page(GTK_NOTEBOOK(backend->notebook), entry->page, label);
    g_object_unref(entry->page); /* the notebook now owns the ref */
    entry->dock_state = TERMINAL_DOCKED;
    gtk_widget_show_all(entry->page);
    focus_page(backend, entry->page);
}

void focus_or_reopen_terminal_tab(GtkBackend *backend, uint64_t session_id) {
    TerminalEntry *entry = find_terminal_entry_by_session_id(backend, session_id);
    if (!entry) {
        return;
    }
    switch (entry->dock_state) {
    case TERMINAL_DOCKED:
        focus_page(backend, entry->page);
        return;
    case TERMINAL_POPPED_OUT:
        /* Bring the existing standalone window forward rather than
         * forcing it back into the notebook - popping back in is its
         * own deliberate action (the "Pop In" button), not implied by
         * "open". */
        gtk_window_present(GTK_WINDOW(entry->popout_window));
        return;
    case TERMINAL_UNDOCKED:
        redock_terminal_entry(backend, entry);
        return;
    }
}

typedef struct TerminalPopoutContext {
    GtkBackend *backend;
    uint64_t session_id;
} TerminalPopoutContext;

static void on_terminal_popout_pop_in_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    TerminalPopoutContext *ctx = user_data;
    TerminalEntry *entry = find_terminal_entry_by_session_id(ctx->backend, ctx->session_id);
    if (entry) {
        redock_terminal_entry(ctx->backend, entry);
    }
}

/* The popout window's own x - same "closing never destroys the session"
 * policy a terminal tab's x already follows (see undock_terminal_tab):
 * this only ever demotes the terminal back to Objects-panel-only
 * tracking, never ends it. Runs on delete-event (not destroy) so the
 * live page can be pulled out *before* GTK tears the window down with
 * it still inside - returning FALSE afterward lets the now-empty window
 * close normally. */
static gboolean on_terminal_popout_delete_event(GtkWidget *window, GdkEvent *event, gpointer user_data) {
    (void)event;
    TerminalPopoutContext *ctx = user_data;
    TerminalEntry *entry = find_terminal_entry_by_session_id(ctx->backend, ctx->session_id);
    if (entry && entry->dock_state == TERMINAL_POPPED_OUT) {
        GtkWidget *container = gtk_widget_get_parent(entry->page);
        g_object_ref(entry->page); /* survive removal - now held only by this extra ref, like any undocked entry */
        gtk_container_remove(GTK_CONTAINER(container), entry->page);
        entry->popout_window = NULL;
        entry->dock_state = TERMINAL_UNDOCKED;
    }
    (void)window;
    return FALSE;
}

/* Called from the tab's own right-click menu ("Pop Out", ui_gtk_tab_labels.c,
 * TAB_TYPE_TERMINAL only) - only valid while docked. Moves page into a
 * new standalone GtkWindow, same ref-before-remove dance
 * undock_terminal_tab uses but reparenting into a real window instead
 * of nowhere. */
void pop_out_terminal_tab(GtkBackend *backend, GtkWidget *page) {
    TerminalEntry *entry = find_terminal_entry_by_page(backend, page);
    if (!entry || entry->dock_state != TERMINAL_DOCKED) {
        return;
    }
    int page_num = gtk_notebook_page_num(GTK_NOTEBOOK(backend->notebook), page);
    if (page_num < 0) {
        return;
    }
    Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");

    g_object_ref(page); /* survive removal until the popout window's own box re-adds it below */
    gtk_notebook_remove_page(GTK_NOTEBOOK(backend->notebook), page_num);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), tab->title);
    gtk_window_set_default_size(GTK_WINDOW(window), 640, 420);
    g_object_set_data(G_OBJECT(window), "workbench-terminal-popout-window", window);
    /* No set_transient_for (unlike the Search window) - a popped-out
     * terminal is meant to behave as a fully independent window, not a
     * utility peer of the main one. gtk_application_add_window() still
     * shares the app's own "quit once every window is gone" bookkeeping
     * - see ui_gtk_search.c's own comment on why that matters. */
    gtk_application_add_window(backend->gtk_app, GTK_WINDOW(window));

    TerminalPopoutContext *ctx = g_new(TerminalPopoutContext, 1);
    ctx->backend = backend;
    ctx->session_id = entry->session->id;
    g_object_set_data_full(G_OBJECT(window), "workbench-terminal-popout-context", ctx, g_free);
    g_signal_connect(window, "delete-event", G_CALLBACK(on_terminal_popout_delete_event), ctx);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 4);
    GtkWidget *pop_in_button = gtk_button_new_with_label("Pop In");
    g_signal_connect(pop_in_button, "clicked", G_CALLBACK(on_terminal_popout_pop_in_clicked), ctx);
    gtk_box_pack_end(GTK_BOX(toolbar), pop_in_button, FALSE, FALSE, 0);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(box), toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), page, TRUE, TRUE, 0);
    g_object_unref(page); /* the box now owns the ref taken above */
    gtk_container_add(GTK_CONTAINER(window), box);

    entry->popout_window = window;
    entry->dock_state = TERMINAL_POPPED_OUT;

    gtk_widget_show_all(window);
    gtk_widget_grab_focus(terminal_get_widget(entry->view));
}

void destroy_terminal_object(GtkBackend *backend, uint64_t session_id) {
    TerminalEntry *entry = find_terminal_entry_by_session_id(backend, session_id);
    if (!entry) {
        return;
    }
    Terminal *view = entry->view;
    GtkWidget *page = entry->page;
    Tab *tab = g_object_get_data(G_OBJECT(page), "workbench-tab");
    Workspace *workspace = g_object_get_data(G_OBJECT(page), "workbench-workspace");

    /* Docked/popped-out: take our own ref before removing so page
     * survives its current container's teardown, matching the ref
     * undock_terminal_tab()/pop_out_terminal_tab() already take for
     * their own transitions. TERMINAL_UNDOCKED already holds it. */
    if (entry->dock_state == TERMINAL_DOCKED) {
        int page_num = gtk_notebook_page_num(GTK_NOTEBOOK(backend->notebook), page);
        g_object_ref(page);
        gtk_notebook_remove_page(GTK_NOTEBOOK(backend->notebook), page_num);
    } else if (entry->dock_state == TERMINAL_POPPED_OUT) {
        GtkWidget *popout_window = entry->popout_window;
        g_object_ref(page);
        gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(page)), page);
        gtk_widget_destroy(popout_window);
    }

    remove_terminal_entry(backend, view); /* frees entry itself */
    terminal_destroy(view);
    terminal_session_destroy((TerminalSession *)tab->backend_data);
    tab->backend_data = NULL;
    workspace_close_tab(workspace, tab->id);

    gtk_widget_destroy(page);
    g_object_unref(page);
}
/* --- end Terminal object lifecycle -------------------------------------- */
