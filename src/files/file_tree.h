#ifndef TOOLBOX_FILE_TREE_H
#define TOOLBOX_FILE_TREE_H

#include <stdbool.h>

#include "file_ids.h"
#include "workspace_root.h"

typedef enum FileNodeType {
    FILE_NODE_DIRECTORY,
    FILE_NODE_REGULAR,
    FILE_NODE_SYMLINK,
    FILE_NODE_OTHER
} FileNodeType;

typedef struct FileTreeNode {
    FileNodeId id;
    FileNodeId parent_id;

    char name[256];
    char relative_path[4096]; /* root-relative; resolved through
                                * workspace_root_resolve_path() only when
                                * an absolute path is actually needed */

    FileNodeType type;

    bool expanded;
    bool children_loaded;
    bool hidden;    /* name starts with '.' - not filtered yet, just
                      * recorded for a future "show hidden" toggle */
    bool read_only;
    bool executable;
} FileTreeNode;

/* Sentinel parent_id meaning "the workspace root itself" - same "0
 * means none" idiom Tab/Workspace already use elsewhere. */
#define FILE_TREE_ROOT_ID ((FileNodeId)0)

typedef struct FileTree FileTree;

FileTree *file_tree_create(const WorkspaceRoot *root);
void file_tree_destroy(FileTree *tree);

/*
 * Scans parent_id's immediate contents (FILE_TREE_ROOT_ID for the
 * workspace root itself) - single level, sorted directories-first then
 * alphabetically. Replaces any previously-loaded children of parent_id
 * (and their own loaded descendants, if any) - safe to call again as a
 * refresh.
 *
 * If the directory can't be opened (permission denied, etc.), adds one
 * synthetic, non-expandable error node instead of failing, so callers
 * never need special-case handling. A symlink is never treated as
 * expandable - calling this on a symlink's id returns -1, same as an
 * id that doesn't exist.
 *
 * Returns the number of children added (>= 1 in the error case), or -1
 * if parent_id itself doesn't exist or isn't a directory.
 */
int file_tree_load_children(FileTree *tree, FileNodeId parent_id);

int file_tree_child_count(const FileTree *tree, FileNodeId parent_id);
const FileTreeNode *file_tree_get_child_at(const FileTree *tree, FileNodeId parent_id, int index);
const FileTreeNode *file_tree_find(const FileTree *tree, FileNodeId id);

#endif /* TOOLBOX_FILE_TREE_H */
