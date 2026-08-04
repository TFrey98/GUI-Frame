#ifndef TOOLBOX_FILE_WATCH_EVENT_H
#define TOOLBOX_FILE_WATCH_EVENT_H

typedef enum FileWatchEventType {
    FILE_WATCH_CREATED,
    FILE_WATCH_MODIFIED,
    FILE_WATCH_DELETED,
    FILE_WATCH_RENAMED,
} FileWatchEventType;

/* Convention: FILE_WATCH_RENAMED populates both old_relative_path and
 * new_relative_path. Every other type only populates new_relative_path
 * (the affected path) and leaves old_relative_path empty ("").
 * Both paths are root-relative, matching every other path this
 * subsystem hands around (FileTreeNode, EditorDocument). */
typedef struct FileWatchEvent {
    FileWatchEventType type;
    char old_relative_path[4096];
    char new_relative_path[4096];
} FileWatchEvent;

#endif /* TOOLBOX_FILE_WATCH_EVENT_H */
