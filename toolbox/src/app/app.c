#include "app.h"

#include <stdlib.h>

#include "../core/workspace.h"
#include "../db/database.h"
#include "../listeners/listener_system.h"
#include "../tools/tool_registry.h"
#include "../tools/toolkit_index.h"
#include "../ui/workbench.h"

struct App {
    int argc;
    char **argv;
    Workspace *workspace;
    ListenerSystem *listener_system;
    Workbench *workbench;
    int db_open;
};

App *app_create(int argc, char **argv) {
    App *app = calloc(1, sizeof(App));
    app->argc = argc;
    app->argv = argv;
    /* A failed open just means app->db_open stays false; the rest of the
     * app is expected to run fine with persistence unavailable, so this
     * isn't treated as a fatal error. */
    app->db_open = (database_open("toolbox.db") == 0);

    /* Registries must exist before workbench_create, since building the
     * initial UI (sidebar tree, etc.) reads from them. */
    tool_registry_init();
    toolkit_index_init();
    app->workspace = workspace_create();
    app->listener_system = listener_system_create();
    app->workbench = workbench_create(app->workspace, app->listener_system);

    return app;
}

int app_run(App *app) {
    return workbench_run(app->workbench, app->argc, app->argv);
}

ListenerSystem *app_get_listener_system(const App *app) {
    return app->listener_system;
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
