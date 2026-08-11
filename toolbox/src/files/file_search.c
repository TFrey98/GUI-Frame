#include "file_search.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* strncasecmp() is POSIX and properly declared via <string.h> in this
 * project's toolchain - NOT strcasestr(), which is a GNU extension that
 * isn't reliably declared even under -std=gnu11 and silently falls back
 * to an implicit int-returning declaration, truncating the returned
 * pointer to 32 bits and segfaulting. HTTP header matching uses the same
 * portable manual-scan approach for this reason. */
static const char *case_insensitive_strstr(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return haystack;
    }
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, needle_len) == 0) {
            return p;
        }
    }
    return NULL;
}

typedef struct ScanState {
    const char *query;
    bool case_sensitive;
    FileSearchMatch *out_matches;
    int max_matches;
    int count;
} ScanState;

static void record_match(ScanState *state, const char *relative_path, bool is_dir) {
    if (state->count >= state->max_matches) {
        return;
    }
    FileSearchMatch *out = &state->out_matches[state->count];
    snprintf(out->relative_path, sizeof(out->relative_path), "%s", relative_path);
    out->is_dir = is_dir;
    state->count++;
}

static void scan_directory(ScanState *state, const char *absolute_dir, const char *relative_dir) {
    if (state->count >= state->max_matches) {
        return;
    }
    DIR *dir = opendir(absolute_dir);
    if (!dir) {
        return;
    }

    struct dirent *entry;
    while (state->count < state->max_matches && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char absolute_child[4400], relative_child[4400];
        snprintf(absolute_child, sizeof(absolute_child), "%s/%s", absolute_dir, entry->d_name);
        if (relative_dir[0] != '\0') {
            snprintf(relative_child, sizeof(relative_child), "%s/%s", relative_dir, entry->d_name);
        } else {
            snprintf(relative_child, sizeof(relative_child), "%s", entry->d_name);
        }

        struct stat st;
        if (lstat(absolute_child, &st) != 0) {
            continue;
        }

        const char *match = state->case_sensitive ? strstr(entry->d_name, state->query)
                                                    : case_insensitive_strstr(entry->d_name, state->query);
        if (match) {
            record_match(state, relative_child, S_ISDIR(st.st_mode));
        }

        /* A matched directory is still recursed into - matching its own
         * name doesn't stop it from also containing further matches.
         * lstat() reports a symlink's own type (S_ISLNK), never the
         * target's, so a real directory symlink never satisfies
         * S_ISDIR() here - never followed into scanning outside the
         * workspace, even though the symlink entry's own name was still
         * eligible to match just above. */
        if (S_ISDIR(st.st_mode)) {
            scan_directory(state, absolute_child, relative_child);
        }
    }
    closedir(dir);
}

int file_search_scan(const WorkspaceRoot *root, const char *query, bool case_sensitive, FileSearchMatch *out_matches,
                      int max_matches) {
    if (!query || query[0] == '\0' || max_matches <= 0) {
        return 0;
    }
    ScanState state = {query, case_sensitive, out_matches, max_matches, 0};
    scan_directory(&state, root->canonical_path, "");
    return state.count;
}
