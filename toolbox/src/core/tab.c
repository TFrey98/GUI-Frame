#include "tab.h"

#include <stdlib.h>
#include <string.h>

static uint64_t g_next_tab_id = 1;

Tab *tab_create(TabType type, const char *title) {
    Tab *tab = malloc(sizeof(Tab));
    tab->id = g_next_tab_id++;
    tab->type = type;
    tab->title = strdup(title ? title : "Untitled");
    tab->backend_data = NULL;
    return tab;
}

void tab_destroy(Tab *tab) {
    if (!tab) {
        return;
    }
    free(tab->title);
    free(tab);
}

void tab_set_title(Tab *tab, const char *title) {
    free(tab->title);
    tab->title = strdup(title);
}
