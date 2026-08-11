#include "ui_gtk_backend.h"

/* --- ui_gtk_dialogs.c: shared dialog helpers ---------------------------
 * Genuinely shared primitives every other dialog file
 * (ui_gtk_explorer_dialogs.c, ui_gtk_listener_dialog.c,
 * ui_gtk_run_dialog.c) calls - error-message formatters, the generic
 * error dialog, and the add_form_row/add_error_row grid-row helpers. */

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
