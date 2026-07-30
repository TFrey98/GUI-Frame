#ifndef TOOLBOX_UI_PLATFORM_H
#define TOOLBOX_UI_PLATFORM_H

/* Forward declaration, not #include "workbench.h" - would be circular,
 * since workbench.h itself includes this header. Both files declaring
 * the same opaque typedef is fine under C11 (this project's standard)
 * as long as the declarations are identical. */
typedef struct Workbench Workbench;

/*
 * Implemented by the active platform UI backend (see src/ui/gtk/).
 * Workbench owns the opaque backend handle but never touches the native
 * GUI toolkit directly - only the platform backend does.
 */
void *platform_ui_create(Workbench *workbench);
int platform_ui_run(void *backend, int argc, char **argv);
void platform_ui_destroy(void *backend);

#endif /* TOOLBOX_UI_PLATFORM_H */
