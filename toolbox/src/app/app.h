#ifndef TOOLBOX_APP_H
#define TOOLBOX_APP_H

#include "../files/workspace_root.h"
#include "../listeners/listener_system.h"

typedef struct App App;

/* Top-level composition root. App creates the model/services before the UI,
 * then destroys them in dependency order after the UI event loop exits. */

/* Returns NULL if the file workspace root (see workspace_root.h) can't
 * be created or accessed - callers must treat that as a fatal startup
 * failure. */
App *app_create(int argc, char **argv);
int app_run(App *app);
/* Accepts NULL. Any successfully-created App must be destroyed exactly once. */
void app_destroy(App *app);

/* Same accessor pattern as workbench_get_listener_system() - lets a
 * white-box test (or any future caller outside the GTK backend) reach
 * the registry directly instead of only through GTK-visible state. */
ListenerSystem *app_get_listener_system(const App *app);

const WorkspaceRoot *app_get_file_workspace_root(const App *app);
const WorkspaceRoot *app_get_toolkit_workspace_root(const App *app);

#endif /* TOOLBOX_APP_H */
