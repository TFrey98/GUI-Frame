#include "workbench.h"

#include <stdlib.h>

struct Workbench {
    Workspace *workspace;
    void *backend;
};

Workbench *workbench_create(Workspace *workspace) {
    Workbench *workbench = calloc(1, sizeof(Workbench));
    workbench->workspace = workspace;
    workbench->backend = platform_ui_create(workbench);
    return workbench;
}

void workbench_destroy(Workbench *workbench) {
    if (!workbench) {
        return;
    }
    platform_ui_destroy(workbench->backend);
    workbench->backend = NULL;
    free(workbench);
}

int workbench_run(Workbench *workbench, int argc, char **argv) {
    return platform_ui_run(workbench->backend, argc, argv);
}

Workspace *workbench_get_workspace(const Workbench *workbench) {
    return workbench->workspace;
}
