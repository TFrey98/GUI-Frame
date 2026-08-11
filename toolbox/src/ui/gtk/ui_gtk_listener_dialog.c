#include "ui_gtk_backend.h"

#include <stdlib.h>
#include <string.h>

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
