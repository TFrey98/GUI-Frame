#ifndef TOOLBOX_WORKBENCH_H
#define TOOLBOX_WORKBENCH_H

#include "../core/workspace.h"
#include "../listeners/listener_system.h"

typedef struct Workbench Workbench;

Workbench *workbench_create(Workspace *workspace, ListenerSystem *listener_system);
void workbench_destroy(Workbench *workbench);

/* Blocks until the UI quits; returns a process exit status. */
int workbench_run(Workbench *workbench, int argc, char **argv);

Workspace *workbench_get_workspace(const Workbench *workbench);
ListenerSystem *workbench_get_listener_system(const Workbench *workbench);

/*
 * Implemented by the active platform backend (see src/platform/linux).
 * Workbench owns the opaque backend handle but never touches the native
 * GUI toolkit directly - only the platform backend does.
 */
void *platform_ui_create(Workbench *workbench);
int platform_ui_run(void *backend, int argc, char **argv);
void platform_ui_destroy(void *backend);

#endif /* TOOLBOX_WORKBENCH_H */
