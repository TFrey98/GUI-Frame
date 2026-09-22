#include "app_paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Resolved on first use and reused for the process lifetime: state is 0
 * before the first call, 1 once g_data_dir holds a usable directory, and
 * -1 once resolution has failed (so a failure isn't retried on every
 * path lookup). */
static char g_data_dir[4096];
static int g_data_dir_state = 0;

/* mkdir() every component of path in turn, tolerating the ones that
 * already exist. Only used for paths this module builds itself, all of
 * which are absolute. */
static bool mkdir_parents(const char *path) {
    char work[4096];
    if (strlen(path) >= sizeof(work)) {
        return false;
    }
    strcpy(work, path);

    for (char *p = work + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(work, 0755) != 0 && errno != EEXIST) {
            return false;
        }
        *p = '/';
    }

    return mkdir(work, 0755) == 0 || errno == EEXIST;
}

/* The directory containing the running executable, via /proc/self/exe -
 * the same derivation workspace_root.c and tools_index.c used directly
 * before this module existed. */
static bool resolve_exe_dir(char *out, size_t out_size) {
    char exe_path[4096];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) {
        return false;
    }
    exe_path[len] = '\0';

    char *last_slash = strrchr(exe_path, '/');
    if (!last_slash || last_slash == exe_path) {
        return false;
    }
    *last_slash = '\0';

    if (strlen(exe_path) >= out_size) {
        return false;
    }
    strcpy(out, exe_path);
    return true;
}

/* $XDG_DATA_HOME/workbench, falling back to the spec's default of
 * $HOME/.local/share when XDG_DATA_HOME is unset or not absolute. */
static bool resolve_user_data_dir(char *out, size_t out_size) {
    const char *xdg = getenv("XDG_DATA_HOME");
    int written;

    if (xdg && xdg[0] == '/') {
        written = snprintf(out, out_size, "%s/workbench", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') {
            return false;
        }
        written = snprintf(out, out_size, "%s/.local/share/workbench", home);
    }

    if (written < 0 || (size_t)written >= out_size) {
        return false;
    }
    return mkdir_parents(out);
}

const char *app_paths_data_dir(void) {
    if (g_data_dir_state != 0) {
        return g_data_dir_state > 0 ? g_data_dir : NULL;
    }

    /* Exe-dir first, and only when it's actually writable: that keeps the
     * development layout (build/workbench, and the smoke tests in
     * build/tests) behaving exactly as it did, and sends only the
     * installed, read-only case (/usr/bin/workbench) to the home
     * directory. */
    if (resolve_exe_dir(g_data_dir, sizeof(g_data_dir)) && access(g_data_dir, W_OK) == 0) {
        g_data_dir_state = 1;
        return g_data_dir;
    }

    if (resolve_user_data_dir(g_data_dir, sizeof(g_data_dir))) {
        g_data_dir_state = 1;
        return g_data_dir;
    }

    g_data_dir[0] = '\0';
    g_data_dir_state = -1;
    return NULL;
}

/* Shared by the two public accessors - joins name onto the data dir,
 * failing rather than truncating if the result doesn't fit. */
static bool join_data_path(const char *name, char *out, size_t out_size) {
    const char *base = app_paths_data_dir();
    if (!base || !name || !out) {
        return false;
    }

    int written = snprintf(out, out_size, "%s/%s", base, name);
    return written >= 0 && (size_t)written < out_size;
}

bool app_paths_data_subdir(const char *name, char *out, size_t out_size) {
    if (!join_data_path(name, out, out_size)) {
        return false;
    }
    if (mkdir(out, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

bool app_paths_data_file(const char *name, char *out, size_t out_size) {
    return join_data_path(name, out, out_size);
}
