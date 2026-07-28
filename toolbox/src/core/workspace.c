#include "workspace.h"

#include <stdlib.h>

/* Fixed-capacity array rather than a dynamic list - simple and fast for the
 * tab counts a single window realistically has; workspace_add_tab just
 * fails past this cap instead of growing. */
#define WORKSPACE_MAX_TABS 64

struct Workspace {
    Tab *tabs[WORKSPACE_MAX_TABS];
    int tab_count;
    /* 0 means "no active tab" - Tab ids start at 1, so 0 is never a real id. */
    uint64_t active_tab_id;
};

Workspace *workspace_create(void) {
    return calloc(1, sizeof(Workspace));
}

void workspace_destroy(Workspace *workspace) {
    if (!workspace) {
        return;
    }
    for (int i = 0; i < workspace->tab_count; i++) {
        tab_destroy(workspace->tabs[i]);
    }
    free(workspace);
}

static int find_index(const Workspace *workspace, uint64_t tab_id) {
    for (int i = 0; i < workspace->tab_count; i++) {
        if (workspace->tabs[i]->id == tab_id) {
            return i;
        }
    }
    return -1;
}

int workspace_add_tab(Workspace *workspace, Tab *tab) {
    if (!tab || workspace->tab_count >= WORKSPACE_MAX_TABS) {
        return -1;
    }
    workspace->tabs[workspace->tab_count++] = tab;
    workspace->active_tab_id = tab->id;
    return 0;
}

int workspace_close_tab(Workspace *workspace, uint64_t tab_id) {
    int index = find_index(workspace, tab_id);
    if (index < 0) {
        return -1;
    }

    tab_destroy(workspace->tabs[index]);
    /* Shift the remaining tabs down to close the gap, keeping the array
     * dense - simpler than tracking free slots for a list this small. */
    for (int i = index; i < workspace->tab_count - 1; i++) {
        workspace->tabs[i] = workspace->tabs[i + 1];
    }
    workspace->tab_count--;

    if (workspace->active_tab_id == tab_id) {
        if (workspace->tab_count == 0) {
            workspace->active_tab_id = 0;
        } else {
            /* Closing the active tab activates whatever slid into its old
             * index (the "next" tab), or the new last tab if it was the
             * last one open - matches how most tabbed UIs pick a successor. */
            int next_index = index < workspace->tab_count ? index : workspace->tab_count - 1;
            workspace->active_tab_id = workspace->tabs[next_index]->id;
        }
    }

    return 0;
}

int workspace_set_active_tab(Workspace *workspace, uint64_t tab_id) {
    if (find_index(workspace, tab_id) < 0) {
        return -1;
    }
    workspace->active_tab_id = tab_id;
    return 0;
}

Tab *workspace_get_active_tab(Workspace *workspace) {
    if (workspace->active_tab_id == 0) {
        return NULL;
    }
    int index = find_index(workspace, workspace->active_tab_id);
    return index < 0 ? NULL : workspace->tabs[index];
}

Tab *workspace_find_tab(const Workspace *workspace, uint64_t tab_id) {
    int index = find_index(workspace, tab_id);
    return index < 0 ? NULL : workspace->tabs[index];
}

int workspace_tab_count(const Workspace *workspace) {
    return workspace->tab_count;
}

Tab *workspace_get_tab_at(const Workspace *workspace, int index) {
    if (index < 0 || index >= workspace->tab_count) {
        return NULL;
    }
    return workspace->tabs[index];
}
