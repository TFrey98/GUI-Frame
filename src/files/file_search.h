#ifndef TOOLBOX_FILE_SEARCH_H
#define TOOLBOX_FILE_SEARCH_H

#include <stdbool.h>

#include "workspace_root.h"

typedef struct FileSearchMatch {
    char relative_path[4096];
    bool is_dir;
} FileSearchMatch;

/*
 * Recursively scans root for files/folders whose own name (not full
 * path) contains query - never reads a file's contents, so this stays
 * cheap regardless of file size or count. Never follows a symlink into
 * scanning outside the workspace (mirrors file_operations.c's/
 * file_tree.c's own discipline) - a symlink entry is itself still
 * name-matchable (checking its name is never unsafe), just never
 * recursed into. A matched directory is still recursed into - matching
 * its own name doesn't stop it from also containing further matches.
 * Case-insensitive matching uses a hand-written strncasecmp()-based
 * scan, not strcasestr() - see file_search.c's own comment on why.
 * Writes up to max_matches FileSearchMatch entries into out_matches
 * (caller-owned) and returns how many were written; stops early rather
 * than growing unbounded if a scan would produce more.
 */
int file_search_scan(const WorkspaceRoot *root, const char *query, bool case_sensitive, FileSearchMatch *out_matches,
                      int max_matches);

#endif /* TOOLBOX_FILE_SEARCH_H */
