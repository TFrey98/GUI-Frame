#include "app/app.h"

int main(int argc, char **argv) {
    App *app = app_create(argc, argv);
    if (!app) {
        return 1;
    }
    int status = app_run(app);
    app_destroy(app);
    return status;
}
