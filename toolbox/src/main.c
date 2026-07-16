#include "app/app.h"

int main(int argc, char **argv) {
    App *app = app_create(argc, argv);
    int status = app_run(app);
    app_destroy(app);
    return status;
}
