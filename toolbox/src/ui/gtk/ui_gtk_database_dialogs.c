#include "ui_gtk_backend.h"

#include <stdio.h>
#include <string.h>

#include "database.h"

/* --- ui_gtk_database_dialogs.c: Export/Clear Database dialogs ----------
 * "Export Database" writes every captured terminal_events row as JSON or
 * YAML; "Clear Database" empties the table. Both are top-bar buttons (see
 * ui_gtk_window.c's build_top_bar). Export follows the same "no native
 * GtkFileChooserDialog, path confined to the file WorkspaceRoot" shape
 * Save As already established (open_save_as_dialog, ui_gtk_editor.c);
 * Clear follows the same GTK_MESSAGE_QUESTION confirmation shape the
 * Revert button already uses. */

typedef enum ExportFormat { EXPORT_FORMAT_JSON, EXPORT_FORMAT_YAML } ExportFormat;

typedef struct ExportBuildContext {
    GString *out;
    ExportFormat format;
    gboolean first;
} ExportBuildContext;

/* Appends data[0..len) as one double-quoted JSON string literal (also
 * valid inside YAML, which accepts the same escapes for a double-quoted
 * scalar) - operates on the raw byte length throughout, never on a
 * NUL-terminated copy, since captured terminal bytes may contain embedded
 * NULs. Invalid UTF-8 bytes are replaced one byte at a time with �,
 * the standard lossy-decode recovery, so binary/ANSI-heavy output can
 * never produce a broken export. */
static void append_quoted_bytes(GString *out, const void *data, size_t len) {
    const gchar *p = data;
    const gchar *end = p + len;
    g_string_append_c(out, '"');
    while (p < end) {
        gunichar ch = g_utf8_get_char_validated(p, end - p);
        if (ch == (gunichar)-1 || ch == (gunichar)-2) {
            g_string_append(out, "\\ufffd");
            p += 1;
            continue;
        }
        const gchar *next = g_utf8_next_char(p);
        switch (ch) {
            case '"':
                g_string_append(out, "\\\"");
                break;
            case '\\':
                g_string_append(out, "\\\\");
                break;
            case '\n':
                g_string_append(out, "\\n");
                break;
            case '\r':
                g_string_append(out, "\\r");
                break;
            case '\t':
                g_string_append(out, "\\t");
                break;
            default:
                if (ch < 0x20) {
                    g_string_append_printf(out, "\\u%04x", ch);
                } else {
                    g_string_append_len(out, p, next - p);
                }
        }
        p = next;
    }
    g_string_append_c(out, '"');
}

static void export_row_cb(uint64_t id, const char *terminal_kind, uint64_t terminal_id, const char *direction,
                           int64_t captured_at, const void *data, size_t data_len, void *user_data) {
    ExportBuildContext *ctx = user_data;

    if (ctx->format == EXPORT_FORMAT_JSON) {
        if (!ctx->first) {
            g_string_append(ctx->out, ",\n");
        }
        ctx->first = FALSE;
        g_string_append(ctx->out, "  {\"id\":");
        g_string_append_printf(ctx->out, "%llu", (unsigned long long)id);
        g_string_append(ctx->out, ",\"terminal_kind\":");
        append_quoted_bytes(ctx->out, terminal_kind, strlen(terminal_kind));
        g_string_append_printf(ctx->out, ",\"terminal_id\":%llu", (unsigned long long)terminal_id);
        g_string_append(ctx->out, ",\"direction\":");
        append_quoted_bytes(ctx->out, direction, strlen(direction));
        g_string_append_printf(ctx->out, ",\"captured_at\":%lld", (long long)captured_at);
        g_string_append(ctx->out, ",\"data\":");
        append_quoted_bytes(ctx->out, data, data_len);
        g_string_append(ctx->out, "}");
    } else {
        g_string_append_printf(ctx->out, "- id: %llu\n", (unsigned long long)id);
        g_string_append(ctx->out, "  terminal_kind: ");
        append_quoted_bytes(ctx->out, terminal_kind, strlen(terminal_kind));
        g_string_append_printf(ctx->out, "\n  terminal_id: %llu\n", (unsigned long long)terminal_id);
        g_string_append(ctx->out, "  direction: ");
        append_quoted_bytes(ctx->out, direction, strlen(direction));
        g_string_append_printf(ctx->out, "\n  captured_at: %lld\n", (long long)captured_at);
        g_string_append(ctx->out, "  data: ");
        append_quoted_bytes(ctx->out, data, data_len);
        g_string_append_c(ctx->out, '\n');
    }
}

static gboolean export_database_to_file(const char *path, ExportFormat format) {
    GString *out = g_string_new(NULL);
    if (format == EXPORT_FORMAT_JSON) {
        g_string_append(out, "[\n");
    }

    ExportBuildContext ctx = {.out = out, .format = format, .first = TRUE};
    if (database_for_each_terminal_event(export_row_cb, &ctx) < 0) {
        g_string_free(out, TRUE);
        return FALSE;
    }

    if (format == EXPORT_FORMAT_JSON) {
        g_string_append(out, "\n]\n");
    }

    /* A one-shot dump, not a live document - a plain write is enough,
     * unlike editor_document_save's atomic temp-file/rename dance. */
    FILE *f = fopen(path, "wb");
    if (!f) {
        g_string_free(out, TRUE);
        return FALSE;
    }
    gboolean ok = fwrite(out->str, 1, out->len, f) == out->len;
    fclose(f);
    g_string_free(out, TRUE);
    return ok;
}

/* --- Export Database dialog --------------------------------------------- */

typedef struct ExportDialogState {
    GtkBackend *backend;
    GtkWidget *path_entry;
    GtkWidget *json_radio;
    GtkWidget *error_label;
} ExportDialogState;

/* The format selection, not whatever the user typed, decides the file
 * extension - strips a trailing .json/.yaml/.yml from the entered name
 * first (so re-typing the same name after switching formats doesn't
 * produce "export.json.yaml") and appends the correct one for format. */
static void build_export_filename(const char *entered_name, ExportFormat format, char *out, size_t out_size) {
    static const char *const known_extensions[] = {".json", ".yaml", ".yml"};
    size_t name_len = strlen(entered_name);
    for (size_t i = 0; i < G_N_ELEMENTS(known_extensions); i++) {
        size_t ext_len = strlen(known_extensions[i]);
        if (name_len > ext_len && g_ascii_strcasecmp(entered_name + name_len - ext_len, known_extensions[i]) == 0) {
            name_len -= ext_len;
            break;
        }
    }
    const char *extension = format == EXPORT_FORMAT_JSON ? ".json" : ".yaml";
    g_snprintf(out, out_size, "%.*s%s", (int)name_len, entered_name, extension);
}

static void on_export_database_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    ExportDialogState *state = user_data;
    if (response_id != GTK_RESPONSE_OK) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }

    ExportFormat format =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->json_radio)) ? EXPORT_FORMAT_JSON : EXPORT_FORMAT_YAML;
    const char *entered_name = gtk_entry_get_text(GTK_ENTRY(state->path_entry));
    char filename[4096];
    build_export_filename(entered_name, format, filename, sizeof(filename));

    char resolved[4096];
    const WorkspaceRoot *root = workbench_get_file_workspace_root(state->backend->workbench);
    if (!workspace_root_resolve_path(root, filename, resolved, sizeof(resolved))) {
        gtk_label_set_text(GTK_LABEL(state->error_label), "That location isn't allowed.");
        return; /* leave the dialog open so the user can fix it */
    }

    if (!export_database_to_file(resolved, format)) {
        gtk_label_set_text(GTK_LABEL(state->error_label), "Could not write the export file.");
        return;
    }

    gtk_widget_destroy(GTK_WIDGET(dialog));
}

void on_export_database_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkBackend *backend = user_data;
    GtkWindow *parent = gtk_application_get_active_window(backend->gtk_app);

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Export Database", parent, GTK_DIALOG_MODAL, "_Cancel",
                                                      GTK_RESPONSE_CANCEL, "_Export", GTK_RESPONSE_OK, NULL);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid, TRUE, TRUE, 0);

    ExportDialogState *state = g_new0(ExportDialogState, 1);
    state->backend = backend;

    state->path_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(state->path_entry), "terminal_history_export");
    add_form_row(GTK_GRID(grid), 0, "File Name", state->path_entry);
    g_object_set_data(G_OBJECT(state->path_entry), "toolbox-export-database-path-entry", state->path_entry);

    GtkWidget *format_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->json_radio = gtk_radio_button_new_with_label(NULL, "JSON");
    GtkWidget *yaml_radio = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(state->json_radio), "YAML");
    g_object_set_data(G_OBJECT(yaml_radio), "toolbox-export-database-yaml-radio", yaml_radio);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->json_radio), TRUE);
    gtk_box_pack_start(GTK_BOX(format_box), state->json_radio, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(format_box), yaml_radio, FALSE, FALSE, 0);
    add_form_row(GTK_GRID(grid), 1, "Format", format_box);

    state->error_label = add_error_row(GTK_GRID(grid), 2);

    g_signal_connect(dialog, "response", G_CALLBACK(on_export_database_response), state);
    g_object_set_data_full(G_OBJECT(dialog), "toolbox-export-database-dialog-state", state, g_free);

    gtk_widget_show_all(dialog);
}

/* --- Clear Database confirmation ----------------------------------------- */

static void on_clear_database_confirm_response(GtkDialog *dialog, gint response_id, gpointer user_data) {
    GtkBackend *backend = user_data;
    if (response_id == GTK_RESPONSE_YES && database_clear_terminal_events() != 0) {
        show_explorer_error(backend, "Could not clear the database.");
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

void on_clear_database_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkBackend *backend = user_data;
    GtkWindow *parent = gtk_application_get_active_window(backend->gtk_app);

    GtkWidget *dialog =
        gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                "Clear all captured terminal input/output from the database? This cannot be undone.");
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Clear", GTK_RESPONSE_YES);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Cancel", GTK_RESPONSE_CANCEL);
    g_signal_connect(dialog, "response", G_CALLBACK(on_clear_database_confirm_response), backend);
    gtk_widget_show_all(dialog);
}
