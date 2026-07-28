#ifndef TOOLBOX_APP_H
#define TOOLBOX_APP_H

#include "../listeners/listener_system.h"

typedef struct App App;

App *app_create(int argc, char **argv);
int app_run(App *app);
void app_destroy(App *app);

/* Same accessor pattern as workbench_get_listener_system() - lets a
 * white-box test (or any future caller outside the GTK backend) reach
 * the registry directly instead of only through GTK-visible state. */
ListenerSystem *app_get_listener_system(const App *app);

#endif /* TOOLBOX_APP_H */
