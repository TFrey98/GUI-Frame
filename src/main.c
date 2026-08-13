#include "app/app.h"

/* Keep process setup deliberately thin: App owns subsystem ordering and the
 * selected UI backend owns the event loop. This also gives tests a single
 * construction boundary without duplicating production initialization. */
int main(int argc, char **argv) {
    App *app = app_create(argc, argv);
    if (!app) {
        return 1;
    }
    int status = app_run(app);
    app_destroy(app);
    return status;
}
