#ifndef TOOLBOX_APP_H
#define TOOLBOX_APP_H

typedef struct App App;

App *app_create(int argc, char **argv);
int app_run(App *app);
void app_destroy(App *app);

#endif /* TOOLBOX_APP_H */
