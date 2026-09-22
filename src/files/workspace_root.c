#include "workspace_root.h"

#include "core/app_paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Resolves <data_dir>/files, creating it if missing. The data dir is
 * the exe dir for a development build and the per-user XDG data dir for
 * an installed one - see app_paths.h. Named "files" rather than
 * "workbench" - the executable itself is named "workbench", so a
 * same-named sibling directory would collide with it on every rebuild. */
static bool resolve_workbench_dir(char *dir_out, size_t dir_out_size) {
    return app_paths_data_subdir("files", dir_out, dir_out_size);
}

/* Shared by workspace_root_init() and workspace_root_init_at() - realpath()s
 * dir and populates both path fields from the result. */
static bool populate_from_dir(WorkspaceRoot *out, const char *dir) {
    char canonical[4096];
    if (!realpath(dir, canonical)) {
        return false;
    }
    if (strlen(canonical) >= sizeof(out->canonical_path)) {
        return false;
    }

    strcpy(out->canonical_path, canonical);
    strcpy(out->display_path, canonical);
    return true;
}

bool workspace_root_init(WorkspaceRoot *out) {
    char dir[4096];
    if (!resolve_workbench_dir(dir, sizeof(dir))) {
        return false;
    }
    return populate_from_dir(out, dir);
}

bool workspace_root_init_at(WorkspaceRoot *out, const char *absolute_directory) {
    if (!absolute_directory) {
        return false;
    }
    return populate_from_dir(out, absolute_directory);
}

/* Rejects a ".." path component anywhere, unconditionally - even a
 * self-cancelling case like "notes/../file.txt" - rather than resolving
 * and rechecking against the root afterward. */
static bool has_dotdot_component(const char *relative_path) {
    const char *p = relative_path;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') {
            p++;
        }
        if (p - start == 2 && start[0] == '.' && start[1] == '.') {
            return true;
        }
        if (*p == '/') {
            p++;
        }
    }
    return false;
}

/* Checks the actual boundary character after the prefix match, so a
 * sibling directory like "<root>-other" can never be mistaken for
 * something inside root. */
static bool path_is_contained(const char *canonical_root, const char *candidate) {
    size_t root_len = strlen(canonical_root);
    if (strncmp(candidate, canonical_root, root_len) != 0) {
        return false;
    }
    char next = candidate[root_len];
    return next == '\0' || next == '/';
}

bool workspace_root_resolve_path(const WorkspaceRoot *root, const char *relative_path, char *resolved_path,
                                  size_t resolved_size) {
    if (!root || !relative_path || !resolved_path || relative_path[0] == '\0') {
        return false;
    }
    if (relative_path[0] == '/') {
        return false;
    }
    if (has_dotdot_component(relative_path)) {
        return false;
    }

    char candidate[8192];
    int written = snprintf(candidate, sizeof(candidate), "%s/%s", root->canonical_path, relative_path);
    if (written < 0 || (size_t)written >= sizeof(candidate)) {
        return false;
    }

    char resolved[4096];
    if (realpath(candidate, resolved) == NULL) {
        if (errno != ENOENT) {
            return false;
        }

        /* Target doesn't exist yet (e.g. a not-yet-created file). Resolve
         * just the parent directory, which must exist, and append the
         * final component - so a symlink anywhere in an *existing*
         * ancestor is still caught, even though the leaf itself isn't
         * there to canonicalize. */
        char parent[8192];
        strcpy(parent, candidate);
        char *last_slash = strrchr(parent, '/');
        if (!last_slash || last_slash == parent) {
            return false;
        }
        *last_slash = '\0';
        const char *leaf = last_slash + 1;
        if (leaf[0] == '\0') {
            return false;
        }

        char resolved_parent[4096];
        if (!realpath(parent, resolved_parent)) {
            return false;
        }

        int leaf_written = snprintf(resolved, sizeof(resolved), "%s/%s", resolved_parent, leaf);
        if (leaf_written < 0 || (size_t)leaf_written >= sizeof(resolved)) {
            return false;
        }
    }

    if (!path_is_contained(root->canonical_path, resolved)) {
        return false;
    }

    size_t resolved_len = strlen(resolved);
    if (resolved_len >= resolved_size) {
        return false;
    }
    strcpy(resolved_path, resolved);
    return true;
}
