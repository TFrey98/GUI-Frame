#ifndef WORKBENCH_FILE_WATCH_QUEUE_H
#define WORKBENCH_FILE_WATCH_QUEUE_H

#include "file_watch_event.h"

typedef struct FileWatchQueue FileWatchQueue;

FileWatchQueue *file_watch_queue_create(void);
void file_watch_queue_destroy(FileWatchQueue *queue);

/* Thread-safe from any thread - the watcher's worker thread pushes
 * events as inotify reports them; never blocks. */
void file_watch_queue_push(FileWatchQueue *queue, FileWatchEvent event);

/* Thread-safe; intended for the GUI thread to drain each tick. Never
 * blocks. Returns 0 and leaves *out untouched if the queue was empty,
 * 1 if an event was popped into *out. */
int file_watch_queue_try_pop(FileWatchQueue *queue, FileWatchEvent *out);

#endif /* WORKBENCH_FILE_WATCH_QUEUE_H */
