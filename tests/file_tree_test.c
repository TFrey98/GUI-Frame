/*
 * Exercises FileTree - the stable-id-backed, single-level-lazy-loading
 * tree model behind the "TOOLBOX" root of the merged explorer sidebar:
 * sorted (dirs-first-then-alpha) non-recursive scans, refresh not
 * duplicating or leaking orphaned grandchildren, an unreadable
 * directory producing a synthetic error node instead of failing, and a
 * symlink being classified without ever being treated as expandable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files/file_tree.h"
#include "files/workspace_root.h"

static void write_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (f) {
        fclose(f);
    }
}

static const FileTreeNode *find_by_name(const FileTree *tree, FileNodeId parent_id, const char *name) {
    int count = file_tree_child_count(tree, parent_id);
    for (int i = 0; i < count; i++) {
        const FileTreeNode *child = file_tree_get_child_at(tree, parent_id, i);
        if (child && strcmp(child->name, name) == 0) {
            return child;
        }
    }
    return NULL;
}

int main(void) {
    char root_template[] = "/tmp/ft_test_root_XXXXXX";
    char *root_dir = mkdtemp(root_template);
    if (!root_dir) {
        fprintf(stderr, "file_tree_test: mkdtemp (root) failed\n");
        return 1;
    }

    char outside_template[] = "/tmp/ft_test_outside_XXXXXX";
    char *outside_dir = mkdtemp(outside_template);
    if (!outside_dir) {
        fprintf(stderr, "file_tree_test: mkdtemp (outside) failed\n");
        return 1;
    }

    WorkspaceRoot root;
    if (!realpath(root_dir, root.canonical_path)) {
        fprintf(stderr, "file_tree_test: realpath(root) failed\n");
        return 1;
    }
    strcpy(root.display_path, root.canonical_path);

    char a_dir[4200], nested_file[4300], b_dir[4200], a_file[4200], restricted_dir[4200], link_path[4200];
    snprintf(a_dir, sizeof(a_dir), "%s/a_dir", root.canonical_path);
    snprintf(nested_file, sizeof(nested_file), "%s/nested.txt", a_dir);
    snprintf(b_dir, sizeof(b_dir), "%s/b_dir", root.canonical_path);
    snprintf(a_file, sizeof(a_file), "%s/a_file.txt", root.canonical_path);
    snprintf(restricted_dir, sizeof(restricted_dir), "%s/restricted", root.canonical_path);
    snprintf(link_path, sizeof(link_path), "%s/link_out", root.canonical_path);

    mkdir(a_dir, 0755);
    write_file(nested_file);
    mkdir(b_dir, 0755);
    write_file(a_file);
    mkdir(restricted_dir, 0755);
    symlink(outside_dir, link_path);

    int status = 0;
    FileTree *tree = file_tree_create(&root);

    /* 1. Root-level load: sorted directories-first, then alphabetical. */
    int n = file_tree_load_children(tree, FILE_TREE_ROOT_ID);
    if (n != 5) {
        fprintf(stderr, "file_tree_test: expected 5 root children, got %d\n", n);
        status = 1;
    }
    const FileTreeNode *c0 = file_tree_get_child_at(tree, FILE_TREE_ROOT_ID, 0);
    const FileTreeNode *c1 = file_tree_get_child_at(tree, FILE_TREE_ROOT_ID, 1);
    const FileTreeNode *c2 = file_tree_get_child_at(tree, FILE_TREE_ROOT_ID, 2);
    const FileTreeNode *c3 = file_tree_get_child_at(tree, FILE_TREE_ROOT_ID, 3);
    const FileTreeNode *c4 = file_tree_get_child_at(tree, FILE_TREE_ROOT_ID, 4);
    if (!c0 || strcmp(c0->name, "a_dir") != 0 || c0->type != FILE_NODE_DIRECTORY) {
        fprintf(stderr, "file_tree_test: expected 'a_dir' directory first\n");
        status = 1;
    }
    if (!c1 || strcmp(c1->name, "b_dir") != 0 || c1->type != FILE_NODE_DIRECTORY) {
        fprintf(stderr, "file_tree_test: expected 'b_dir' directory second\n");
        status = 1;
    }
    if (!c2 || strcmp(c2->name, "restricted") != 0 || c2->type != FILE_NODE_DIRECTORY) {
        fprintf(stderr, "file_tree_test: expected 'restricted' directory third\n");
        status = 1;
    }
    if (!c3 || strcmp(c3->name, "a_file.txt") != 0 || c3->type != FILE_NODE_REGULAR) {
        fprintf(stderr, "file_tree_test: expected 'a_file.txt' fourth\n");
        status = 1;
    }
    if (!c4 || strcmp(c4->name, "link_out") != 0 || c4->type != FILE_NODE_SYMLINK) {
        fprintf(stderr, "file_tree_test: expected 'link_out' symlink fifth\n");
        status = 1;
    }
    if (!c3 || strcmp(c3->relative_path, "a_file.txt") != 0) {
        fprintf(stderr, "file_tree_test: relative_path should be root-relative, not absolute\n");
        status = 1;
    }

    FileNodeId a_dir_id = c0 ? c0->id : 0;

    /* 2. Loading a subdirectory is non-recursive. */
    int nested_count = file_tree_load_children(tree, a_dir_id);
    if (nested_count != 1) {
        fprintf(stderr, "file_tree_test: expected 1 child under a_dir, got %d\n", nested_count);
        status = 1;
    }
    const FileTreeNode *nested = file_tree_get_child_at(tree, a_dir_id, 0);
    if (!nested || strcmp(nested->name, "nested.txt") != 0 || nested->type != FILE_NODE_REGULAR) {
        fprintf(stderr, "file_tree_test: expected 'nested.txt' under a_dir\n");
        status = 1;
    }
    if (!nested || strcmp(nested->relative_path, "a_dir/nested.txt") != 0) {
        fprintf(stderr, "file_tree_test: expected nested.txt's relative_path to be 'a_dir/nested.txt'\n");
        status = 1;
    }
    FileNodeId nested_id = nested ? nested->id : 0;
    if (file_tree_child_count(tree, FILE_TREE_ROOT_ID) != 5) {
        fprintf(stderr, "file_tree_test: loading a_dir's children must not surface them at the root level\n");
        status = 1;
    }

    /* 3. Reloading the root (simulated Refresh) doesn't duplicate, and
     * cleanly discards the previously-loaded a_dir/nested.txt subtree
     * rather than leaking it. */
    int n2 = file_tree_load_children(tree, FILE_TREE_ROOT_ID);
    if (n2 != 5) {
        fprintf(stderr, "file_tree_test: refresh should still yield 5 root children, got %d\n", n2);
        status = 1;
    }
    if (file_tree_find(tree, a_dir_id) != NULL) {
        fprintf(stderr, "file_tree_test: stale a_dir id should not resolve after refresh\n");
        status = 1;
    }
    if (file_tree_find(tree, nested_id) != NULL) {
        fprintf(stderr, "file_tree_test: stale nested.txt id should not resolve after refresh (orphaned grandchild leak)\n");
        status = 1;
    }

    /* 4. An unreadable directory yields one synthetic error node instead
     * of crashing or requiring the caller to special-case a failure. */
    if (geteuid() == 0) {
        printf("file_tree_test: skipping permission-denied case (running as root)\n");
    } else {
        chmod(restricted_dir, 0);
        const FileTreeNode *restricted_node = find_by_name(tree, FILE_TREE_ROOT_ID, "restricted");
        if (!restricted_node) {
            fprintf(stderr, "file_tree_test: 'restricted' entry not found after refresh\n");
            status = 1;
        } else {
            int err_count = file_tree_load_children(tree, restricted_node->id);
            if (err_count != 1) {
                fprintf(stderr, "file_tree_test: expected 1 synthetic error node, got %d\n", err_count);
                status = 1;
            }
            const FileTreeNode *err_node = file_tree_get_child_at(tree, restricted_node->id, 0);
            if (!err_node || err_node->type != FILE_NODE_OTHER) {
                fprintf(stderr, "file_tree_test: expected a FILE_NODE_OTHER error node\n");
                status = 1;
            }
        }
        chmod(restricted_dir, 0755);
    }

    /* 5. A symlink is classified without ever being followed, and is
     * never treated as expandable. */
    const FileTreeNode *link_node = find_by_name(tree, FILE_TREE_ROOT_ID, "link_out");
    if (!link_node || link_node->type != FILE_NODE_SYMLINK) {
        fprintf(stderr, "file_tree_test: 'link_out' should be classified FILE_NODE_SYMLINK\n");
        status = 1;
    } else if (file_tree_load_children(tree, link_node->id) != -1) {
        fprintf(stderr, "file_tree_test: a symlink must not be treated as an expandable directory\n");
        status = 1;
    }

    /* 6. A bogus id resolves to nothing. */
    if (file_tree_find(tree, (FileNodeId)999999) != NULL) {
        fprintf(stderr, "file_tree_test: a bogus id should not resolve\n");
        status = 1;
    }

    file_tree_destroy(tree);

    chmod(restricted_dir, 0755);
    unlink(link_path);
    rmdir(restricted_dir);
    unlink(a_file);
    rmdir(b_dir);
    unlink(nested_file);
    rmdir(a_dir);
    rmdir(root.canonical_path);
    rmdir(outside_dir);

    if (status == 0) {
        printf("file_tree_test: sorted non-recursive scans, refresh-without-leaking, permission errors, and "
               "non-expandable symlinks all verified\n");
    }
    return status;
}
