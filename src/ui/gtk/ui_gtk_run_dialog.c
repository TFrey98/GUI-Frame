#include "ui_gtk_backend.h"
#include "ui_gtk_terminal_internal.h"

#include <string.h>

/* --- Run with Arguments dialog -----------------------------------------
 * Collects a real TerminalLaunchRequest and validates working_directory
 * through workspace_root_resolve_path() before ever calling into
 * run_command_in_new_terminal/run_command_in_active_terminal, same
 * "validate before it ever reaches the manager" discipline the New
 * Listener dialog already established. Arguments are parsed via
 * g_shell_parse_argv() (a real argv array, never re-concatenated) and
 * Environment Overrides via a comma-separated KEY=VALUE list. */

typedef struct RunWithArgumentsState {
    GtkBackend *backend;
    const WorkspaceRoot *root; /* which root relative_path/working_directory resolve against */
    char relative_path[4096]; /* the script/executable's own relative path */
    GtkWidget *working_directory_entry, *working_directory_error;
    GtkWidget *arguments_entry, *arguments_error;
    GtkWidget *env_entry, *env_error;
    GtkWidget *create_new_terminal_check;
} RunWithArgumentsState;

static void on_run_with_arguments_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    RunWithArgumentsState *state = user_data;
    if (response_id != GTK_RESPONSE_OK) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }

    gtk_label_set_text(GTK_LABEL(state->working_directory_error), "");
    gtk_label_set_text(GTK_LABEL(state->arguments_error), "");
    gtk_label_set_text(GTK_LABEL(state->env_error), "");

    GtkBackend *backend = state->backend;
    const WorkspaceRoot *root = state->root;

    char resolved_exe[4096];
    if (!workspace_root_resolve_path(root, state->relative_path, resolved_exe, sizeof(resolved_exe))) {
        gtk_label_set_text(GTK_LABEL(state->working_directory_error),
                            "The executable is missing or outside the workspace.");
        return;
    }

    const char *working_directory_text = gtk_entry_get_text(GTK_ENTRY(state->working_directory_entry));
    char resolved_cwd[4096];
    gboolean cwd_ok;
    if (working_directory_text[0] == '\0') {
        g_strlcpy(resolved_cwd, root->canonical_path, sizeof(resolved_cwd));
        cwd_ok = TRUE;
    } else {
        cwd_ok = workspace_root_resolve_path(root, working_directory_text, resolved_cwd, sizeof(resolved_cwd));
    }
    if (!cwd_ok) {
        gtk_label_set_text(GTK_LABEL(state->working_directory_error), "That directory isn't allowed.");
        return;
    }

    gint argc = 0;
    gchar **argv = NULL;
    GError *error = NULL;
    const char *arguments_text = gtk_entry_get_text(GTK_ENTRY(state->arguments_entry));
    if (arguments_text[0] != '\0' && !g_shell_parse_argv(arguments_text, &argc, &argv, &error)) {
        gtk_label_set_text(GTK_LABEL(state->arguments_error), error ? error->message : "Could not parse arguments.");
        g_clear_error(&error);
        return;
    }

    const char *env_text = gtk_entry_get_text(GTK_ENTRY(state->env_entry));
    char **env_overrides = NULL;
    size_t env_override_count = 0;
    if (env_text[0] != '\0') {
        gchar **parts = g_strsplit(env_text, ",", -1);
        size_t count = g_strv_length(parts);
        env_overrides = g_new0(char *, count + 1);
        for (size_t i = 0; i < count; i++) {
            gchar *trimmed = g_strstrip(g_strdup(parts[i]));
            if (!strchr(trimmed, '=')) {
                gtk_label_set_text(GTK_LABEL(state->env_error), "Each override must look like KEY=VALUE.");
                g_free(trimmed);
                g_strfreev(parts);
                g_strfreev(env_overrides);
                if (argv) {
                    g_strfreev(argv);
                }
                return;
            }
            env_overrides[env_override_count++] = trimmed;
        }
        g_strfreev(parts);
    }

    TerminalLaunchRequest request = {0};
    g_strlcpy(request.executable, resolved_exe, sizeof(request.executable));
    g_strlcpy(request.working_directory, resolved_cwd, sizeof(request.working_directory));
    request.arguments = argv;
    request.argument_count = (size_t)argc;
    request.create_new_terminal = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->create_new_terminal_check));

    if (request.create_new_terminal) {
        run_command_in_new_terminal(backend, &request, env_overrides, env_override_count);
    } else {
        run_command_in_active_terminal(backend, &request, env_overrides, env_override_count);
    }

    if (argv) {
        g_strfreev(argv);
    }
    g_strfreev(env_overrides);

    gtk_widget_destroy(GTK_WIDGET(dialog));
}

void open_run_with_arguments_dialog(GtkBackend *backend, const WorkspaceRoot *root, const char *relative_path,
                                     GtkWindow *parent) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Run with Arguments", parent, GTK_DIALOG_MODAL, "_Cancel",
                                                      GTK_RESPONSE_CANCEL, "_Run", GTK_RESPONSE_OK, NULL);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid, TRUE, TRUE, 0);

    RunWithArgumentsState *state = g_new0(RunWithArgumentsState, 1);
    state->backend = backend;
    state->root = root;
    g_strlcpy(state->relative_path, relative_path, sizeof(state->relative_path));

    int row = 0;
    add_form_row(GTK_GRID(grid), row++, "Executable", gtk_label_new(relative_path));

    char parent_dir[4096] = "";
    const char *slash = strrchr(relative_path, '/');
    if (slash) {
        size_t len = (size_t)(slash - relative_path);
        if (len >= sizeof(parent_dir)) {
            len = sizeof(parent_dir) - 1;
        }
        memcpy(parent_dir, relative_path, len);
        parent_dir[len] = '\0';
    }
    state->working_directory_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(state->working_directory_entry), parent_dir);
    add_form_row(GTK_GRID(grid), row++, "Working Directory", state->working_directory_entry);
    state->working_directory_error = add_error_row(GTK_GRID(grid), row++);

    state->arguments_entry = gtk_entry_new();
    add_form_row(GTK_GRID(grid), row++, "Arguments", state->arguments_entry);
    state->arguments_error = add_error_row(GTK_GRID(grid), row++);

    state->env_entry = gtk_entry_new();
    add_form_row(GTK_GRID(grid), row++, "Environment Overrides", state->env_entry);
    state->env_error = add_error_row(GTK_GRID(grid), row++);

    state->create_new_terminal_check = gtk_check_button_new_with_label("Create new terminal");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->create_new_terminal_check), TRUE);
    gtk_grid_attach(GTK_GRID(grid), state->create_new_terminal_check, 0, row++, 2, 1);

    g_signal_connect(dialog, "response", G_CALLBACK(on_run_with_arguments_response), state);
    g_object_set_data_full(G_OBJECT(dialog), "workbench-run-with-arguments-state", state, g_free);

    g_object_set_data(G_OBJECT(state->working_directory_entry), "workbench-run-working-directory-entry",
                       state->working_directory_entry);
    g_object_set_data(G_OBJECT(state->arguments_entry), "workbench-run-arguments-entry", state->arguments_entry);
    g_object_set_data(G_OBJECT(state->env_entry), "workbench-run-env-entry", state->env_entry);
    g_object_set_data(G_OBJECT(state->create_new_terminal_check), "workbench-run-create-new-terminal-check",
                       state->create_new_terminal_check);
    g_object_set_data(G_OBJECT(dialog), "workbench-run-with-arguments-dialog", dialog);

    gtk_widget_show_all(dialog);
}
/* --- end Run with Arguments dialog --------------------------------------- */
