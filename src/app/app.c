#include "app.h"

#include <stdlib.h>

#include "../core/workspace.h"
#include "../db/database.h"
#include "../listeners/listener_system.h"
#include "../tools/tool_registry.h"
#include "../tools/toolkit_index.h"
#include "../ui/workbench.h"

struct App {
    int argc;    /* borrowed from main(); valid for the process lifetime */
    char **argv; /* borrowed alongside argc */
    Workspace *workspace;             /* owned */
    ListenerSystem *listener_system;   /* owned */
    Workbench *workbench;              /* owned; borrows the fields above */
    WorkspaceRoot file_workspace_root;
    WorkspaceRoot toolkit_workspace_root;
    int db_open; /* records whether the optional global DB needs closing */
};

App *app_create(int argc, char **argv) {
    App *app = calloc(1, sizeof(App));
    app->argc = argc;
    app->argv = argv;

    /* Unlike db_open below, a failure here is fatal: every later
     * filesystem-subsystem phase assumes a valid, already-resolved
     * workspace root exists. */
    if (!workspace_root_init(&app->file_workspace_root)) {
        free(app);
        return NULL;
    }

    /* A failed open just means app->db_open stays false; the rest of the
     * app is expected to run fine with persistence unavailable, so this
     * isn't treated as a fatal error. */
    app->db_open = (database_open("workbench.db") == 0);
    if (app->db_open) {
        database_init_schema();
    }

    /* Registries must exist before workbench_create, since building the
     * initial UI (sidebar tree, etc.) reads from them. */
    tool_registry_init();
    toolkit_index_init();

    /* toolkit_index_init() already created toolkit/ if it was missing -
     * same "assume a valid, already-resolved root" fatality as
     * file_workspace_root above, since the explorer sidebar's Toolkit
     * section now routes real create/rename/delete/save operations
     * through this root too, not just reads. */
    if (!workspace_root_init_at(&app->toolkit_workspace_root, toolkit_index_dir())) {
        if (app->db_open) {
            database_close();
        }
        toolkit_index_shutdown();
        tool_registry_shutdown();
        free(app);
        return NULL;
    }

    app->workspace = workspace_create();
    app->listener_system = listener_system_create();
    app->workbench = workbench_create(app->workspace, app->listener_system, &app->file_workspace_root,
                                       &app->toolkit_workspace_root);

    return app;
}

int app_run(App *app) {
    return workbench_run(app->workbench, app->argc, app->argv);
}

ListenerSystem *app_get_listener_system(const App *app) {
    return app->listener_system;
}

const WorkspaceRoot *app_get_file_workspace_root(const App *app) {
    return &app->file_workspace_root;
}

const WorkspaceRoot *app_get_toolkit_workspace_root(const App *app) {
    return &app->toolkit_workspace_root;
}

void app_destroy(App *app) {
    if (!app) {
        return;
    }

    /* Workbench first: it owns the live GTK window and any tabs/terminals
     * still open, which may reference workspace and toolkit_index state
     * while tearing themselves down. Tearing those down after the
     * registries/workspace they depend on would leave dangling references. */
    workbench_destroy(app->workbench);
    app->workbench = NULL;

    /* Same reasoning: the debug UI (torn down inside workbench_destroy
     * above, via its tick source being removed) borrows this pointer,
     * so it must outlive that call. */
    listener_system_destroy(app->listener_system);
    app->listener_system = NULL;

    tool_registry_shutdown();
    toolkit_index_shutdown();

    workspace_destroy(app->workspace);
    app->workspace = NULL;

    if (app->db_open) {
        database_close();
        app->db_open = 0;
    }

    free(app);
}
