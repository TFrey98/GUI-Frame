#ifndef WORKBENCH_TAB_H
#define WORKBENCH_TAB_H

#include <stdint.h>

typedef enum TabType {
    TAB_TYPE_TERMINAL,
    TAB_TYPE_REPORT,
    TAB_TYPE_SCRIPT,
    TAB_TYPE_LISTENER,
    TAB_TYPE_CONNECTION_TERMINAL,
    TAB_TYPE_EDITOR,
    TAB_TYPE_BINARY_INFO
} TabType;

/*
 * backend_data is owned and interpreted by whichever backend attaches it
 * (e.g. a Terminal* once a terminal tab spawns a real shell) - Tab itself
 * never touches it. A void* is not ideal long term, but it's an acceptable
 * bridge while tab types don't have real backends yet.
 */
typedef struct Tab {
    uint64_t id;
    TabType type;
    char *title;
    void *backend_data;
} Tab;

Tab *tab_create(TabType type, const char *title);
void tab_destroy(Tab *tab);

void tab_set_title(Tab *tab, const char *title);

#endif /* WORKBENCH_TAB_H */
