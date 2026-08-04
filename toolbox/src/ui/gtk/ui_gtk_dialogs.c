#include "ui_gtk_internal.h"

#include <stdlib.h>
#include <string.h>

const char *file_operation_error_message(FileOperationResult result) {
    switch (result) {
        case FILE_OP_ALREADY_EXISTS:
            return "An item with that name already exists.";
        case FILE_OP_NOT_FOUND:
            return "That item no longer exists.";
        case FILE_OP_PERMISSION_DENIED:
            return "Permission denied.";
        case FILE_OP_OUTSIDE_WORKSPACE:
            return "That name isn't allowed.";
        case FILE_OP_DIRECTORY_NOT_EMPTY:
            return "That folder isn't empty.";
        case FILE_OP_INVALID_NAME:
            return "That name isn't allowed.";
        case FILE_OP_OK:
            return "";
        case FILE_OP_IO_ERROR:
        default:
            return "The operation failed.";
    }
}

const char *editor_save_error_message(EditorSaveResult result) {
    switch (result) {
        case EDITOR_SAVE_OUTSIDE_WORKSPACE:
            return "That location isn't allowed.";
        case EDITOR_SAVE_INVALID_NAME:
            return "That name isn't allowed.";
        case EDITOR_SAVE_ALREADY_EXISTS:
            return "An item with that name already exists.";
        case EDITOR_SAVE_READ_ONLY:
            return "This file is read-only and can't be saved.";
        case EDITOR_SAVE_OK:
            return "";
        case EDITOR_SAVE_IO_ERROR:
        default:
            return "Could not save this file.";
    }
}

static void on_explorer_error_dialog_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    (void)response_id;
    (void)user_data;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

void show_explorer_error(GtkBackend *backend, const char *message) {
    GtkWindow *parent = gtk_application_get_active_window(backend->gtk_app);
    GtkWidget *dialog =
        gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", message);
    g_signal_connect(dialog, "response", G_CALLBACK(on_explorer_error_dialog_response), NULL);
    gtk_widget_show_all(dialog);
}

/* Returns the row's label widget - only URL Path/Host Header currently
 * need it (to hide the label along with its field for non-HTTP types);
 * every other call site just discards it. */
GtkWidget *add_form_row(GtkGrid *grid, int row, const char *label_text, GtkWidget *entry) {
    GtkWidget *label = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_grid_attach(grid, label, 0, row, 1, 1);
    gtk_grid_attach(grid, entry, 1, row, 1, 1);
    gtk_widget_set_hexpand(entry, TRUE);
    return label;
}

GtkWidget *add_error_row(GtkGrid *grid, int row) {
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_grid_attach(grid, label, 1, row, 1, 1);
    return label;
}

/* Triggered from the explorer's context menu, but its whole effect is
 * unconditionally showing a dialog - lives here rather than
 * ui_gtk_menus.c, see the restructuring plan's splitting rule. Only its
 * G_CALLBACK reference lives in menus.c's menu-building code. */
static void on_explorer_properties_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    (void)response_id;
    (void)user_data;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

void on_explorer_menu_properties(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    ExplorerMenuContext *ctx = user_data;
    GtkBackend *backend = ctx->backend;
    GtkTreeStore *store = backend->explorer_store;

    gchar *name = NULL;
    gchar *relative_path = NULL;
    gboolean is_dir = FALSE;
    guint64 node_id = 0;
    int source = EXPLORER_SOURCE_FILES;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &ctx->iter, EXPLORER_COL_NAME, &name, EXPLORER_COL_PATH,
                        &relative_path, EXPLORER_COL_IS_DIR, &is_dir, EXPLORER_COL_NODE_ID, &node_id,
                        EXPLORER_COL_SOURCE, &source, -1);

    gboolean read_only, executable;
    if (source == EXPLORER_SOURCE_FILES) {
        const FileTreeNode *node = file_tree_find(backend->file_tree, (FileNodeId)node_id);
        read_only = node && node->read_only;
        executable = node && node->executable;
    } else {
        bool stat_executable = false, stat_read_only = true;
        const WorkspaceRoot *root = explorer_root_for_source(backend, source);
        char resolved[4096];
        if (relative_path && relative_path[0] != '\0' &&
            workspace_root_resolve_path(root, relative_path, resolved, sizeof(resolved))) {
            explorer_toolkit_file_flags(resolved, &stat_executable, &stat_read_only);
        }
        read_only = stat_read_only;
        executable = stat_executable;
    }

    GtkWindow *parent = gtk_application_get_active_window(backend->gtk_app);
    GtkWidget *dialog =
        gtk_dialog_new_with_buttons("Properties", parent, GTK_DIALOG_MODAL, "_OK", GTK_RESPONSE_OK, NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_container_add(GTK_CONTAINER(content), grid);

    int row = 0;
    add_form_row(GTK_GRID(grid), row++, "Name", gtk_label_new(name ? name : ""));
    add_form_row(GTK_GRID(grid), row++, "Type", gtk_label_new(is_dir ? "Folder" : "File"));
    add_form_row(GTK_GRID(grid), row++, "Path",
                 gtk_label_new((relative_path && relative_path[0]) ? relative_path : "/"));
    add_form_row(GTK_GRID(grid), row++, "Read-only", gtk_label_new(read_only ? "Yes" : "No"));
    add_form_row(GTK_GRID(grid), row++, "Executable", gtk_label_new(executable ? "Yes" : "No"));

    g_signal_connect(dialog, "response", G_CALLBACK(on_explorer_properties_response), NULL);
    gtk_widget_show_all(dialog);

    g_free(name);
    g_free(relative_path);
}

/* --- New Listener dialog --------------------------------------------
 * Collects a real ListenerConfig and validates it (listener_config_validate())
 * before ever calling into ListenerManager, so invalid input never
 * reaches it - only a config that already passed gets handed to
 * listener_manager_create_listener(). Callback Host auto-follows Bind
 * Address until the user edits Callback Host directly; URL Path/Host
 * Header/Certificate/Key Path show only for the relevant Type. */

typedef struct ListenerDialogState {
    GtkBackend *backend;
    GtkWidget *name_entry, *name_error;
    GtkWidget *type_combo; /* index 0 = Reverse TCP, 1 = HTTP, 2 = HTTPS */
    GtkWidget *bind_address_entry, *bind_address_error;
    GtkWidget *port_entry, *port_error;
    GtkWidget *callback_host_entry, *callback_host_error;
    /* HTTP or HTTPS only - all four hidden otherwise, including the row
     * labels (add_form_row's own label isn't otherwise reachable to
     * toggle). */
    GtkWidget *url_path_label, *url_path_entry, *url_path_error;
    GtkWidget *host_header_label, *host_header_entry, *host_header_error;
    /* HTTPS only - same hidden-unless-relevant treatment. */
    GtkWidget *cert_path_label, *cert_path_entry, *cert_path_error;
    GtkWidget *key_path_label, *key_path_entry, *key_path_error;
    GtkWidget *general_error; /* ENDPOINT and any other whole-config error */
    gboolean callback_host_dirty;    /* true once the user has typed into it directly */
    gboolean updating_callback_host; /* guards the auto-follow's own programmatic set
                                       * from being mistaken for a user edit */
    gboolean submitting; /* re-entrancy guard - not structurally reachable in this
                           * single-threaded, non-nested-loop GTK flow, but cheap
                           * insurance per the doc's explicit ask */
} ListenerDialogState;

static void on_bind_address_changed(GtkEditable *editable, gpointer user_data) {
    (void)editable;
    ListenerDialogState *state = user_data;
    if (state->callback_host_dirty) {
        return;
    }
    const char *bind_text = gtk_entry_get_text(GTK_ENTRY(state->bind_address_entry));
    state->updating_callback_host = TRUE;
    gtk_entry_set_text(GTK_ENTRY(state->callback_host_entry), bind_text);
    state->updating_callback_host = FALSE;
}

static void on_callback_host_changed(GtkEditable *editable, gpointer user_data) {
    (void)editable;
    ListenerDialogState *state = user_data;
    if (!state->updating_callback_host) {
        state->callback_host_dirty = TRUE;
    }
}

/* URL Path/Host Header apply to HTTP or HTTPS; Certificate/Key Path
 * apply to HTTPS only - shown only when relevant to the selected type. */
static void on_listener_type_changed(GtkComboBox *combo, gpointer user_data) {
    ListenerDialogState *state = user_data;
    gint active = gtk_combo_box_get_active(combo);
    gboolean is_http_family = active == 1 || active == 2;
    gboolean is_https = active == 2;
    gtk_widget_set_visible(state->url_path_label, is_http_family);
    gtk_widget_set_visible(state->url_path_entry, is_http_family);
    gtk_widget_set_visible(state->url_path_error, is_http_family);
    gtk_widget_set_visible(state->host_header_label, is_http_family);
    gtk_widget_set_visible(state->host_header_entry, is_http_family);
    gtk_widget_set_visible(state->host_header_error, is_http_family);
    gtk_widget_set_visible(state->cert_path_label, is_https);
    gtk_widget_set_visible(state->cert_path_entry, is_https);
    gtk_widget_set_visible(state->cert_path_error, is_https);
    gtk_widget_set_visible(state->key_path_label, is_https);
    gtk_widget_set_visible(state->key_path_entry, is_https);
    gtk_widget_set_visible(state->key_path_error, is_https);
}

/* Non-numeric or out-of-range text both become 0, which
 * listener_config_validate() already flags as "Port must be between 1
 * and 65535" - no separate UI-side numeric check needed. */
static uint16_t parse_port(const char *text) {
    if (!text || !*text) {
        return 0;
    }
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 1 || value > 65535) {
        return 0;
    }
    return (uint16_t)value;
}

static GtkWidget *error_label_for_field(ListenerDialogState *state, ListenerConfigField field) {
    switch (field) {
        case LISTENER_CONFIG_FIELD_NAME: return state->name_error;
        case LISTENER_CONFIG_FIELD_BIND_ADDRESS: return state->bind_address_error;
        case LISTENER_CONFIG_FIELD_PORT: return state->port_error;
        case LISTENER_CONFIG_FIELD_CALLBACK_HOST: return state->callback_host_error;
        case LISTENER_CONFIG_FIELD_URL_PATH: return state->url_path_error;
        case LISTENER_CONFIG_FIELD_HOST_HEADER: return state->host_header_error;
        case LISTENER_CONFIG_FIELD_CERT_PATH: return state->cert_path_error;
        case LISTENER_CONFIG_FIELD_KEY_PATH: return state->key_path_error;
        default:
            /* TYPE/ENDPOINT: no dedicated widget in this dialog - TYPE
             * never fires since the combo only ever offers types
             * validate() accepts. */
            return state->general_error;
    }
}

static void clear_dialog_errors(ListenerDialogState *state) {
    gtk_label_set_text(GTK_LABEL(state->name_error), "");
    gtk_label_set_text(GTK_LABEL(state->bind_address_error), "");
    gtk_label_set_text(GTK_LABEL(state->port_error), "");
    gtk_label_set_text(GTK_LABEL(state->callback_host_error), "");
    gtk_label_set_text(GTK_LABEL(state->url_path_error), "");
    gtk_label_set_text(GTK_LABEL(state->host_header_error), "");
    gtk_label_set_text(GTK_LABEL(state->cert_path_error), "");
    gtk_label_set_text(GTK_LABEL(state->key_path_error), "");
    gtk_label_set_text(GTK_LABEL(state->general_error), "");
}

static void on_listener_dialog_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    ListenerDialogState *state = user_data;

    if (response_id != GTK_RESPONSE_OK) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }
    if (state->submitting) {
        return;
    }
    state->submitting = TRUE;
    clear_dialog_errors(state);

    gint active = gtk_combo_box_get_active(GTK_COMBO_BOX(state->type_combo));
    gboolean is_http_family = active == 1 || active == 2;
    gboolean is_https = active == 2;

    ListenerConfig config = {0};
    config.name = strdup(gtk_entry_get_text(GTK_ENTRY(state->name_entry)));
    config.type = is_https ? LISTENER_TYPE_HTTPS : (is_http_family ? LISTENER_TYPE_HTTP : LISTENER_TYPE_REVERSE_TCP);
    config.bind_address = strdup(gtk_entry_get_text(GTK_ENTRY(state->bind_address_entry)));
    config.port = parse_port(gtk_entry_get_text(GTK_ENTRY(state->port_entry)));
    config.callback_host = strdup(gtk_entry_get_text(GTK_ENTRY(state->callback_host_entry)));
    if (is_http_family) {
        config.url_path = strdup(gtk_entry_get_text(GTK_ENTRY(state->url_path_entry)));
        config.host_header = strdup(gtk_entry_get_text(GTK_ENTRY(state->host_header_entry)));
    }
    if (is_https) {
        config.cert_path = strdup(gtk_entry_get_text(GTK_ENTRY(state->cert_path_entry)));
        config.key_path = strdup(gtk_entry_get_text(GTK_ENTRY(state->key_path_entry)));
    }

    ObjectRegistry *registry = state->backend->listener_system->registry;
    ListenerConfigValidation validation;
    if (!listener_config_validate(registry, &config, &validation)) {
        for (int i = 0; i < validation.error_count; i++) {
            gtk_label_set_text(GTK_LABEL(error_label_for_field(state, validation.errors[i].field)),
                                validation.errors[i].message);
        }
        free(config.name);
        free(config.bind_address);
        free(config.callback_host);
        free(config.url_path);
        free(config.host_header);
        free(config.cert_path);
        free(config.key_path);
        state->submitting = FALSE;
        return; /* leave the dialog open so the user can fix it */
    }

    uint64_t id = listener_manager_create_listener(state->backend->listener_system->listener_manager, config, NULL);
    if (id == 0) {
        /* Shouldn't happen - nothing else can touch the registry between
         * the validate() call above and this one on this single-threaded
         * GTK flow - but don't silently do nothing if it somehow does. */
        gtk_label_set_text(GTK_LABEL(state->general_error), "Failed to create listener");
        free(config.name);
        free(config.bind_address);
        free(config.callback_host);
        free(config.url_path);
        free(config.host_header);
        free(config.cert_path);
        free(config.key_path);
        state->submitting = FALSE;
        return;
    }

    listener_manager_start_async(state->backend->listener_system->listener_manager, id);
    state->backend->last_listener_id = id;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void open_new_listener_dialog(GtkBackend *backend, GtkWindow *parent) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons("New Listener", parent, GTK_DIALOG_MODAL, "_Cancel",
                                                      GTK_RESPONSE_CANCEL, "_Create", GTK_RESPONSE_OK, NULL);

    ListenerDialogState *state = g_new0(ListenerDialogState, 1);
    state->backend = backend;

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid, TRUE, TRUE, 0);

    int row = 0;

    state->name_entry = gtk_entry_new();
    char default_name[32];
    g_snprintf(default_name, sizeof(default_name), "Listener %d", backend->next_listener_number++);
    gtk_entry_set_text(GTK_ENTRY(state->name_entry), default_name);
    add_form_row(GTK_GRID(grid), row++, "Name", state->name_entry);
    state->name_error = add_error_row(GTK_GRID(grid), row++);

    state->type_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->type_combo), "Reverse TCP");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->type_combo), "HTTP");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->type_combo), "HTTPS");
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->type_combo), 0);
    add_form_row(GTK_GRID(grid), row++, "Type", state->type_combo);

    state->bind_address_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(state->bind_address_entry), "0.0.0.0");
    add_form_row(GTK_GRID(grid), row++, "Bind Address", state->bind_address_entry);
    state->bind_address_error = add_error_row(GTK_GRID(grid), row++);

    state->port_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(state->port_entry), "4444");
    add_form_row(GTK_GRID(grid), row++, "Port", state->port_entry);
    state->port_error = add_error_row(GTK_GRID(grid), row++);

    /* Left blank by default - Bind Address defaults to a wildcard, and
     * the auto-follow below deliberately never propagates a wildcard
     * into Callback Host (it's never a valid callback host). */
    state->callback_host_entry = gtk_entry_new();
    add_form_row(GTK_GRID(grid), row++, "Callback Host", state->callback_host_entry);
    state->callback_host_error = add_error_row(GTK_GRID(grid), row++);

    state->url_path_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(state->url_path_entry), "/");
    state->url_path_label = add_form_row(GTK_GRID(grid), row++, "URL Path", state->url_path_entry);
    state->url_path_error = add_error_row(GTK_GRID(grid), row++);

    state->host_header_entry = gtk_entry_new();
    state->host_header_label = add_form_row(GTK_GRID(grid), row++, "Host Header", state->host_header_entry);
    state->host_header_error = add_error_row(GTK_GRID(grid), row++);

    state->cert_path_entry = gtk_entry_new();
    state->cert_path_label = add_form_row(GTK_GRID(grid), row++, "Certificate Path", state->cert_path_entry);
    state->cert_path_error = add_error_row(GTK_GRID(grid), row++);

    state->key_path_entry = gtk_entry_new();
    state->key_path_label = add_form_row(GTK_GRID(grid), row++, "Private Key Path", state->key_path_entry);
    state->key_path_error = add_error_row(GTK_GRID(grid), row++);

    state->general_error = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->general_error), 0.0);
    gtk_grid_attach(GTK_GRID(grid), state->general_error, 0, row, 2, 1);

    /* Connected after every default gtk_entry_set_text() above, so
     * populating the defaults doesn't itself trigger the auto-follow. */
    g_signal_connect(state->bind_address_entry, "changed", G_CALLBACK(on_bind_address_changed), state);
    g_signal_connect(state->callback_host_entry, "changed", G_CALLBACK(on_callback_host_changed), state);
    g_signal_connect(state->type_combo, "changed", G_CALLBACK(on_listener_type_changed), state);
    g_signal_connect(dialog, "response", G_CALLBACK(on_listener_dialog_response), state);

    g_object_set_data(G_OBJECT(state->name_entry), "toolbox-listener-name-entry", state->name_entry);
    g_object_set_data(G_OBJECT(state->name_error), "toolbox-listener-name-error", state->name_error);
    g_object_set_data(G_OBJECT(state->type_combo), "toolbox-listener-type-combo", state->type_combo);
    g_object_set_data(G_OBJECT(state->bind_address_entry), "toolbox-listener-bind-address-entry",
                       state->bind_address_entry);
    g_object_set_data(G_OBJECT(state->port_entry), "toolbox-listener-port-entry", state->port_entry);
    g_object_set_data(G_OBJECT(state->callback_host_entry), "toolbox-listener-callback-host-entry",
                       state->callback_host_entry);
    g_object_set_data(G_OBJECT(state->url_path_entry), "toolbox-listener-url-path-entry", state->url_path_entry);
    g_object_set_data(G_OBJECT(state->host_header_entry), "toolbox-listener-host-header-entry",
                       state->host_header_entry);
    g_object_set_data(G_OBJECT(state->cert_path_entry), "toolbox-listener-cert-path-entry", state->cert_path_entry);
    g_object_set_data(G_OBJECT(state->key_path_entry), "toolbox-listener-key-path-entry", state->key_path_entry);
    g_object_set_data(G_OBJECT(dialog), "toolbox-new-listener-dialog", dialog);
    /* Frees state automatically when the dialog is destroyed - same
     * ownership pattern TabLabelData uses on its event_box above. */
    g_object_set_data_full(G_OBJECT(dialog), "toolbox-listener-dialog-state", state, g_free);

    gtk_widget_show_all(dialog);
    /* show_all() above forces every child visible, including the
     * HTTP-only rows - re-sync them to the combo's actual (default
     * Reverse TCP) selection now that showing is done. */
    on_listener_type_changed(GTK_COMBO_BOX(state->type_combo), state);
}

void on_new_listener_clicked(GtkButton *button, gpointer user_data) {
    GtkBackend *backend = user_data;
    GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(button));
    open_new_listener_dialog(backend, GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : NULL);
}
/* --- end New Listener dialog ----------------------------------------- */

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
    g_object_set_data_full(G_OBJECT(dialog), "toolbox-run-with-arguments-state", state, g_free);

    g_object_set_data(G_OBJECT(state->working_directory_entry), "toolbox-run-working-directory-entry",
                       state->working_directory_entry);
    g_object_set_data(G_OBJECT(state->arguments_entry), "toolbox-run-arguments-entry", state->arguments_entry);
    g_object_set_data(G_OBJECT(state->env_entry), "toolbox-run-env-entry", state->env_entry);
    g_object_set_data(G_OBJECT(state->create_new_terminal_check), "toolbox-run-create-new-terminal-check",
                       state->create_new_terminal_check);
    g_object_set_data(G_OBJECT(dialog), "toolbox-run-with-arguments-dialog", dialog);

    gtk_widget_show_all(dialog);
}
/* --- end Run with Arguments dialog --------------------------------------- */
