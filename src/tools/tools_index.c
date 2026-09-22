#include "tools_index.h"

#include "core/app_paths.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Single global root-level index, sized to TOOLS_INDEX_MAX_ENTRIES - the
 * sidebar only ever needs one; per-directory scans (tools_scan_directory)
 * write into caller-provided buffers instead of this global state. */
static ToolsIndexEntry g_entries[TOOLS_INDEX_MAX_ENTRIES];
static int g_entry_count = 0;
static char *g_tools_dir = NULL;

static void free_entries(void) {
    for (int i = 0; i < g_entry_count; i++) {
        free(g_entries[i].name);
        free(g_entries[i].path);
        g_entries[i].name = NULL;
        g_entries[i].path = NULL;
    }
    g_entry_count = 0;
}

/* Directories sort before files (matches most file-tree UIs), then
 * alphabetically within each group. */
static int compare_entries(const void *a, const void *b) {
    const ToolsIndexEntry *ea = a;
    const ToolsIndexEntry *eb = b;
    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1;
    }
    return strcmp(ea->name, eb->name);
}

int tools_scan_directory(const char *dir_path, ToolsIndexEntry *out, int max_entries) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return -1;
    }

    int count = 0;
    struct dirent *entry;
    while (count < max_entries && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }

        out[count].name = strdup(entry->d_name);
        out[count].path = strdup(path);
        out[count].is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        count++;
    }
    closedir(dir);

    qsort(out, count, sizeof(ToolsIndexEntry), compare_entries);
    return count;
}

/* Resolves <data_dir>/tools, creating it if missing. The data dir is the
 * exe dir for a development build - matching where CMake's POST_BUILD
 * step creates it - and the per-user XDG data dir for an installed one.
 * See app_paths.h. Returned string is owned by the caller.
 *
 * The directory was called "toolkit" up to 0.1.0~beta, when it was too
 * easily confused with the sidebar's other root; anyone carrying one
 * forward gets it renamed in place on first launch rather than silently
 * losing access to its contents. */
static char *resolve_tools_dir(void) {
    app_paths_rename_legacy_subdir("toolkit", "tools");

    char dir[4096];
    if (!app_paths_data_subdir("tools", dir, sizeof(dir))) {
        return NULL;
    }
    return strdup(dir);
}

void tools_index_init(void) {
    g_entry_count = 0;
    g_tools_dir = resolve_tools_dir();
    tools_index_rescan();
}

void tools_index_shutdown(void) {
    free_entries();
    free(g_tools_dir);
    g_tools_dir = NULL;
}

int tools_index_rescan(void) {
    if (!g_tools_dir) {
        return -1;
    }
    free_entries();
    int count = tools_scan_directory(g_tools_dir, g_entries, TOOLS_INDEX_MAX_ENTRIES);
    g_entry_count = count < 0 ? 0 : count;
    return count;
}

int tools_index_count(void) {
    return g_entry_count;
}

const ToolsIndexEntry *tools_index_get(int index) {
    if (index < 0 || index >= g_entry_count) {
        return NULL;
    }
    return &g_entries[index];
}

const char *tools_index_dir(void) {
    return g_tools_dir;
}
