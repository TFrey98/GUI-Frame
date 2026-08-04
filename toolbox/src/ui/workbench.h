#ifndef TOOLBOX_WORKBENCH_H
#define TOOLBOX_WORKBENCH_H

#include "../core/workspace.h"
#include "../files/workspace_root.h"
#include "../listeners/listener_system.h"
#include "ui_platform.h"

typedef struct Workbench Workbench;

Workbench *workbench_create(Workspace *workspace, ListenerSystem *listener_system,
                             const WorkspaceRoot *file_workspace_root, const WorkspaceRoot *toolkit_workspace_root);
void workbench_destroy(Workbench *workbench);

/* Blocks until the UI quits; returns a process exit status. */
int workbench_run(Workbench *workbench, int argc, char **argv);

Workspace *workbench_get_workspace(const Workbench *workbench);
ListenerSystem *workbench_get_listener_system(const Workbench *workbench);
const WorkspaceRoot *workbench_get_file_workspace_root(const Workbench *workbench);
const WorkspaceRoot *workbench_get_toolkit_workspace_root(const Workbench *workbench);

#endif /* TOOLBOX_WORKBENCH_H */
