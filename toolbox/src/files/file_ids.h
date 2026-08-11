#ifndef TOOLBOX_FILE_IDS_H
#define TOOLBOX_FILE_IDS_H

#include <stdint.h>

/* Distinct aliases document which subsystem an otherwise identical integer
 * belongs to. IDs are process-local, monotonic identities; they are not
 * filesystem inode numbers or stable persistence keys. Zero is reserved for
 * "none/root" where a subsystem needs a sentinel. */
typedef uint64_t FileNodeId;
typedef uint64_t DocumentId;
typedef uint64_t EditorViewId;

#endif /* TOOLBOX_FILE_IDS_H */
