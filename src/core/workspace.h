#ifndef WORKBENCH_WORKSPACE_H
#define WORKBENCH_WORKSPACE_H

#include "tab.h"

typedef struct Workspace Workspace;

/* Toolkit-independent tab model for one application window. Workspace owns
 * every Tab added to it; the GTK notebook is a view over this collection. */

Workspace *workspace_create(void);
void workspace_destroy(Workspace *workspace);

/* Takes ownership of tab and makes it the active tab. Returns 0 on
 * success, -1 if the workspace is full or tab is NULL. */
int workspace_add_tab(Workspace *workspace, Tab *tab);

/* Destroys and removes the tab with the given id, reassigning the active
 * tab if needed. Returns 0 on success, -1 if no such tab exists. */
int workspace_close_tab(Workspace *workspace, uint64_t tab_id);

/* Returns 0 on success, -1 if no such tab exists. */
int workspace_set_active_tab(Workspace *workspace, uint64_t tab_id);

/* Returned pointers are borrowed and become invalid when their tab (or the
 * workspace) is destroyed. */
Tab *workspace_get_active_tab(Workspace *workspace);
Tab *workspace_find_tab(const Workspace *workspace, uint64_t tab_id);

int workspace_tab_count(const Workspace *workspace);
Tab *workspace_get_tab_at(const Workspace *workspace, int index);

#endif /* WORKBENCH_WORKSPACE_H */
