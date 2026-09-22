#ifndef WORKBENCH_TOOLS_INDEX_H
#define WORKBENCH_TOOLS_INDEX_H

/* Shared cap for both the root index and any single tools_scan_directory
 * call (e.g. lazily scanning a subfolder on expand in the sidebar). */
#define TOOLS_INDEX_MAX_ENTRIES 256

typedef struct ToolsIndexEntry {
    char *name; /* basename */
    char *path; /* full resolved path */
    int is_dir;
} ToolsIndexEntry;

/* Resolves the tools/ directory next to the running executable, creates
 * it if missing, and performs an initial scan. */
void tools_index_init(void);
void tools_index_shutdown(void);

/* Re-scans the resolved tools directory. Returns the new entry count, or
 * -1 if the directory could not be read. */
int tools_index_rescan(void);

int tools_index_count(void);
const ToolsIndexEntry *tools_index_get(int index);

/* Resolved absolute path to the tools directory. */
const char *tools_index_dir(void);

/*
 * Non-recursive, single-level scan of dir_path: records each immediate
 * entry (skipping "." and "..") into out, marking directories via is_dir
 * without ever reading their contents. Results are sorted directories-
 * first, then alphabetically within each group. Returns the entry count,
 * or -1 if dir_path could not be opened. Exposed directly so it can be
 * unit-tested against an arbitrary temp directory.
 */
int tools_scan_directory(const char *dir_path, ToolsIndexEntry *out, int max_entries);

#endif /* WORKBENCH_TOOLS_INDEX_H */
