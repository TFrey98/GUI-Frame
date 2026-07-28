#include "workbench.h"

#include <stdlib.h>

struct Workbench {
    Workspace *workspace;
    ListenerSystem *listener_system; /* borrowed - App owns it, outlives the Workbench */
    /* Opaque handle owned by the active platform backend (e.g. GtkBackend
     * in src/platform/linux/ui_gtk.c). Workbench never dereferences it
     * directly - only passes it back through platform_ui_*. */
    void *backend;
};

Workbench *workbench_create(Workspace *workspace, ListenerSystem *listener_system) {
    Workbench *workbench = calloc(1, sizeof(Workbench));
    workbench->workspace = workspace;
    workbench->listener_system = listener_system;
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

ListenerSystem *workbench_get_listener_system(const Workbench *workbench) {
    return workbench->listener_system;
}
