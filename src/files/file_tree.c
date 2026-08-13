#include "file_tree.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Generous cap, same "just stop adding past it" precedent as
 * OBJECT_REGISTRY_MAX_OBJECTS. */
#define FILE_TREE_MAX_NODES 8192

/* Per-directory scan cap - bounds one file_tree_load_children() call,
 * independent of the tree's overall capacity above. */
#define FILE_TREE_MAX_SCAN_ENTRIES 2048

/* Dense array + linear search by id + monotonic ids, same shape as
 * object_registry.c, scaled for a plausibly deep/broad file tree
 * accumulated over a single browsing session. */
struct FileTree {
    WorkspaceRoot root;
    FileTreeNode nodes[FILE_TREE_MAX_NODES];
    int count;
    FileNodeId next_id;
};

FileTree *file_tree_create(const WorkspaceRoot *root) {
    FileTree *tree = calloc(1, sizeof(FileTree));
    tree->root = *root;
    tree->next_id = 1;
    return tree;
}

void file_tree_destroy(FileTree *tree) {
    free(tree);
}

static int find_index(const FileTree *tree, FileNodeId id) {
    for (int i = 0; i < tree->count; i++) {
        if (tree->nodes[i].id == id) {
            return i;
        }
    }
    return -1;
}

const FileTreeNode *file_tree_find(const FileTree *tree, FileNodeId id) {
    int idx = find_index(tree, id);
    return idx >= 0 ? &tree->nodes[idx] : NULL;
}

static void remove_at(FileTree *tree, int index) {
    for (int i = index; i < tree->count - 1; i++) {
        tree->nodes[i] = tree->nodes[i + 1];
    }
    tree->count--;
}

/* Removes id and everything loaded beneath it (a child can itself have
 * loaded grandchildren from a prior expansion). */
static void remove_subtree(FileTree *tree, FileNodeId id) {
    for (int i = 0; i < tree->count;) {
        if (tree->nodes[i].parent_id == id) {
            /* Recursing shifts the array out from under this loop -
             * rescan from the same index instead of advancing. */
            remove_subtree(tree, tree->nodes[i].id);
        } else {
            i++;
        }
    }
    int idx = find_index(tree, id);
    if (idx >= 0) {
        remove_at(tree, idx);
    }
}

static void remove_children(FileTree *tree, FileNodeId parent_id) {
    for (int i = 0; i < tree->count;) {
        if (tree->nodes[i].parent_id == parent_id) {
            remove_subtree(tree, tree->nodes[i].id);
        } else {
            i++;
        }
    }
}

typedef struct ScanEntry {
    char name[256];
    FileNodeType type;
    bool hidden;
    bool read_only;
    bool executable;
} ScanEntry;

/* Directories first, then alphabetical within each group - same
 * ordering toolkit_index.c's compare_entries() already establishes. */
static int compare_scan_entries(const void *a, const void *b) {
    const ScanEntry *ea = a;
    const ScanEntry *eb = b;
    bool a_dir = ea->type == FILE_NODE_DIRECTORY;
    bool b_dir = eb->type == FILE_NODE_DIRECTORY;
    if (a_dir != b_dir) {
        return a_dir ? -1 : 1;
    }
    return strcmp(ea->name, eb->name);
}

/* Classifies via lstat() - symlinks are detected without ever being
 * followed, matching this step's deliberate "never expand a symlink"
 * scope decision. */
static void classify_entry(const char *absolute_path, ScanEntry *entry) {
    struct stat st;
    if (lstat(absolute_path, &st) != 0) {
        entry->type = FILE_NODE_OTHER;
        entry->read_only = true;
        entry->executable = false;
        return;
    }

    if (S_ISLNK(st.st_mode)) {
        entry->type = FILE_NODE_SYMLINK;
        entry->read_only = false;
        entry->executable = false;
    } else if (S_ISDIR(st.st_mode)) {
        entry->type = FILE_NODE_DIRECTORY;
        entry->read_only = (access(absolute_path, W_OK) != 0);
        entry->executable = false;
    } else if (S_ISREG(st.st_mode)) {
        entry->type = FILE_NODE_REGULAR;
        entry->read_only = (access(absolute_path, W_OK) != 0);
        entry->executable = (st.st_mode & S_IXUSR) != 0;
    } else {
        entry->type = FILE_NODE_OTHER;
        entry->read_only = true;
        entry->executable = false;
    }
}

static void append_child(FileTree *tree, FileNodeId parent_id, const char *parent_relative_path,
                          const ScanEntry *entry) {
    if (tree->count >= FILE_TREE_MAX_NODES) {
        return;
    }
    FileTreeNode *node = &tree->nodes[tree->count++];
    memset(node, 0, sizeof(*node));
    node->id = tree->next_id++;
    node->parent_id = parent_id;
    snprintf(node->name, sizeof(node->name), "%s", entry->name);
    if (parent_relative_path[0] == '\0') {
        snprintf(node->relative_path, sizeof(node->relative_path), "%s", entry->name);
    } else {
        snprintf(node->relative_path, sizeof(node->relative_path), "%s/%s", parent_relative_path, entry->name);
    }
    node->type = entry->type;
    node->hidden = entry->hidden;
    node->read_only = entry->read_only;
    node->executable = entry->executable;
}

static void add_error_node(FileTree *tree, FileNodeId parent_id, int error_number) {
    if (tree->count >= FILE_TREE_MAX_NODES) {
        return;
    }
    FileTreeNode *node = &tree->nodes[tree->count++];
    memset(node, 0, sizeof(*node));
    node->id = tree->next_id++;
    node->parent_id = parent_id;
    snprintf(node->name, sizeof(node->name), "(cannot read: %s)", strerror(error_number));
    node->type = FILE_NODE_OTHER;
    node->children_loaded = true; /* never expandable */
    node->read_only = true;
}

int file_tree_load_children(FileTree *tree, FileNodeId parent_id) {
    const char *parent_relative_path = "";
    if (parent_id != FILE_TREE_ROOT_ID) {
        const FileTreeNode *parent = file_tree_find(tree, parent_id);
        if (!parent || parent->type != FILE_NODE_DIRECTORY) {
            return -1;
        }
        parent_relative_path = parent->relative_path;
    }

    char absolute_dir[4096];
    if (parent_relative_path[0] == '\0') {
        snprintf(absolute_dir, sizeof(absolute_dir), "%s", tree->root.canonical_path);
    } else if (!workspace_root_resolve_path(&tree->root, parent_relative_path, absolute_dir, sizeof(absolute_dir))) {
        return -1;
    }

    remove_children(tree, parent_id);

    DIR *dir = opendir(absolute_dir);
    if (!dir) {
        add_error_node(tree, parent_id, errno);
        return 1;
    }

    static ScanEntry entries[FILE_TREE_MAX_SCAN_ENTRIES];
    int entry_count = 0;

    struct dirent *dirent_entry;
    while (entry_count < FILE_TREE_MAX_SCAN_ENTRIES && (dirent_entry = readdir(dir)) != NULL) {
        if (strcmp(dirent_entry->d_name, ".") == 0 || strcmp(dirent_entry->d_name, "..") == 0) {
            continue;
        }

        char absolute_entry_path[4400];
        snprintf(absolute_entry_path, sizeof(absolute_entry_path), "%s/%s", absolute_dir, dirent_entry->d_name);

        ScanEntry *entry = &entries[entry_count];
        snprintf(entry->name, sizeof(entry->name), "%s", dirent_entry->d_name);
        entry->hidden = (dirent_entry->d_name[0] == '.');
        classify_entry(absolute_entry_path, entry);
        entry_count++;
    }
    closedir(dir);

    qsort(entries, (size_t)entry_count, sizeof(ScanEntry), compare_scan_entries);

    for (int i = 0; i < entry_count; i++) {
        append_child(tree, parent_id, parent_relative_path, &entries[i]);
    }

    return entry_count;
}

int file_tree_child_count(const FileTree *tree, FileNodeId parent_id) {
    int count = 0;
    for (int i = 0; i < tree->count; i++) {
        if (tree->nodes[i].parent_id == parent_id) {
            count++;
        }
    }
    return count;
}

const FileTreeNode *file_tree_get_child_at(const FileTree *tree, FileNodeId parent_id, int index) {
    int seen = 0;
    for (int i = 0; i < tree->count; i++) {
        if (tree->nodes[i].parent_id == parent_id) {
            if (seen == index) {
                return &tree->nodes[i];
            }
            seen++;
        }
    }
    return NULL;
}
