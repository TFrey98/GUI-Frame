#include "toolkit_index.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static ToolkitEntry g_entries[TOOLKIT_INDEX_MAX_ENTRIES];
static int g_entry_count = 0;
static char *g_toolkit_dir = NULL;

static void free_entries(void) {
    for (int i = 0; i < g_entry_count; i++) {
        free(g_entries[i].name);
        free(g_entries[i].path);
        g_entries[i].name = NULL;
        g_entries[i].path = NULL;
    }
    g_entry_count = 0;
}

static int compare_entries(const void *a, const void *b) {
    const ToolkitEntry *ea = a;
    const ToolkitEntry *eb = b;
    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1;
    }
    return strcmp(ea->name, eb->name);
}

int toolkit_scan_directory(const char *dir_path, ToolkitEntry *out, int max_entries) {
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

    qsort(out, count, sizeof(ToolkitEntry), compare_entries);
    return count;
}

/* Resolves the directory containing the running executable via
 * /proc/self/exe, so toolkit/ is found next to the binary regardless of
 * the cwd the app was launched from - matching where CMake's POST_BUILD
 * step creates it. */
static char *resolve_exe_relative_toolkit_dir(void) {
    char exe_path[4096];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) {
        return NULL;
    }
    exe_path[len] = '\0';

    char *last_slash = strrchr(exe_path, '/');
    if (!last_slash) {
        return NULL;
    }
    *last_slash = '\0';

    char *dir = malloc(strlen(exe_path) + strlen("/toolkit") + 1);
    sprintf(dir, "%s/toolkit", exe_path);

    mkdir(dir, 0755);

    return dir;
}

void toolkit_index_init(void) {
    g_entry_count = 0;
    g_toolkit_dir = resolve_exe_relative_toolkit_dir();
    toolkit_index_rescan();
}

void toolkit_index_shutdown(void) {
    free_entries();
    free(g_toolkit_dir);
    g_toolkit_dir = NULL;
}

int toolkit_index_rescan(void) {
    if (!g_toolkit_dir) {
        return -1;
    }
    free_entries();
    int count = toolkit_scan_directory(g_toolkit_dir, g_entries, TOOLKIT_INDEX_MAX_ENTRIES);
    g_entry_count = count < 0 ? 0 : count;
    return count;
}

int toolkit_index_count(void) {
    return g_entry_count;
}

const ToolkitEntry *toolkit_index_get(int index) {
    if (index < 0 || index >= g_entry_count) {
        return NULL;
    }
    return &g_entries[index];
}

const char *toolkit_index_dir(void) {
    return g_toolkit_dir;
}
