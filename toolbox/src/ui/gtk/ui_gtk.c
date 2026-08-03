#include "ui_gtk.h"

#include <stdlib.h>

#include "ui_gtk_internal.h"

void *platform_ui_create(Workbench *workbench) {
    GtkBackend *backend = malloc(sizeof(GtkBackend));
    backend->workbench = workbench;
    backend->notebook = NULL;
    backend->next_terminal_number = 1;
    backend->terminal_entries = g_ptr_array_new();
    backend->listener_system = workbench_get_listener_system(workbench);
    backend->file_tree = file_tree_create(workbench_get_file_workspace_root(workbench));
    backend->explorer_store = NULL;
    backend->explorer_tree_view = NULL;
    backend->explorer_name_renderer = NULL;
    backend->explorer_editing_row = NULL;
    backend->last_listener_id = 0;
    backend->status_label = NULL;
    backend->next_listener_number = 1;
    backend->tick_source_id = 0;
    backend->css_provider = NULL;
    backend->dark_mode = FALSE;
    backend->gtk_app = gtk_application_new("dev.toolbox.app", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(backend->gtk_app, "activate", G_CALLBACK(on_activate), backend);
    return backend;
}

int platform_ui_run(void *backend_ptr, int argc, char **argv) {
    GtkBackend *backend = backend_ptr;
    return g_application_run(G_APPLICATION(backend->gtk_app), argc, argv);
}

void platform_ui_destroy(void *backend_ptr) {
    GtkBackend *backend = backend_ptr;
    if (!backend) {
        return;
    }

    /* The main loop has already exited by the time this runs (this is
     * only called after platform_ui_run() returns), so the tick can't
     * actually fire again regardless - removing the source explicitly
     * is just more honest than relying on that implicitly. */
    if (backend->tick_source_id != 0) {
        g_source_remove(backend->tick_source_id);
    }

    /* Individually-closed tabs already removed and freed their entry in
     * on_tab_close_clicked; this covers whatever's left when the whole
     * window closes without every tab being closed first. The View holds
     * its own widget reference (see terminal_create's g_object_ref_sink),
     * so it's still safe to destroy here even though the window - and
     * therefore backend->notebook - is long gone by this point. */
    for (guint i = 0; i < backend->terminal_entries->len; i++) {
        TerminalEntry *entry = g_ptr_array_index(backend->terminal_entries, i);
        terminal_destroy(entry->view);
        terminal_session_destroy(entry->session);
        g_free(entry);
    }
    g_ptr_array_free(backend->terminal_entries, TRUE);

    file_tree_destroy(backend->file_tree);
    if (backend->explorer_editing_row) {
        gtk_tree_row_reference_free(backend->explorer_editing_row);
    }
    if (backend->css_provider) {
        g_object_unref(backend->css_provider);
    }

    g_object_unref(backend->gtk_app);
    free(backend);
}
