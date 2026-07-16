#include "app.h"

#include <stdlib.h>

#include "../core/workspace.h"
#include "../db/database.h"
#include "../tools/tool_registry.h"
#include "../tools/toolkit_index.h"
#include "../ui/workbench.h"

struct App {
    int argc;
    char **argv;
    Workspace *workspace;
    Workbench *workbench;
    int db_open;
};

App *app_create(int argc, char **argv) {
    App *app = calloc(1, sizeof(App));
    app->argc = argc;
    app->argv = argv;
    app->db_open = (database_open("toolbox.db") == 0);

    tool_registry_init();
    toolkit_index_init();
    app->workspace = workspace_create();
    app->workbench = workbench_create(app->workspace);

    return app;
}

int app_run(App *app) {
    return workbench_run(app->workbench, app->argc, app->argv);
}

void app_destroy(App *app) {
    if (!app) {
        return;
    }

    workbench_destroy(app->workbench);
    app->workbench = NULL;

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
