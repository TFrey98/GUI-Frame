#ifndef TOOLBOX_TOOLKIT_INDEX_H
#define TOOLBOX_TOOLKIT_INDEX_H

/* Shared cap for both the root index and any single toolkit_scan_directory
 * call (e.g. lazily scanning a subfolder on expand in the sidebar). */
#define TOOLKIT_INDEX_MAX_ENTRIES 256

typedef struct ToolkitEntry {
    char *name; /* basename */
    char *path; /* full resolved path */
    int is_dir;
} ToolkitEntry;

/* Resolves the toolkit/ directory next to the running executable, creates
 * it if missing, and performs an initial scan. */
void toolkit_index_init(void);
void toolkit_index_shutdown(void);

/* Re-scans the resolved toolkit directory. Returns the new entry count, or
 * -1 if the directory could not be read. */
int toolkit_index_rescan(void);

int toolkit_index_count(void);
const ToolkitEntry *toolkit_index_get(int index);

/* Resolved absolute path to the toolkit directory. */
const char *toolkit_index_dir(void);

/*
 * Non-recursive, single-level scan of dir_path: records each immediate
 * entry (skipping "." and "..") into out, marking directories via is_dir
 * without ever reading their contents. Results are sorted directories-
 * first, then alphabetically within each group. Returns the entry count,
 * or -1 if dir_path could not be opened. Exposed directly so it can be
 * unit-tested against an arbitrary temp directory.
 */
int toolkit_scan_directory(const char *dir_path, ToolkitEntry *out, int max_entries);

#endif /* TOOLBOX_TOOLKIT_INDEX_H */
