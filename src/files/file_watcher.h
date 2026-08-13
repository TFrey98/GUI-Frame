#ifndef WORKBENCH_FILE_WATCHER_H
#define WORKBENCH_FILE_WATCHER_H

#include "file_watch_event.h"
#include "workspace_root.h"

/*
 * Owns one inotify fd and one worker thread on the same self-pipe/
 * poll()/stop-and-join shape tcp_worker.c already established for
 * network listeners - the worker translates raw inotify events into
 * FileWatchEvents and pushes them to an internal FileWatchQueue for the
 * GUI thread to drain each tick via file_watcher_try_pop_event().
 *
 * Only ever watches directories explicitly handed to it via
 * file_watcher_watch_directory() - no upfront recursive watch of the
 * whole tree, mirroring FileTree's own "lazy, only what's been
 * expanded" scope.
 */
typedef struct FileWatcher FileWatcher;

/* root is borrowed and must outlive the watcher - every reported event's
 * paths are resolved relative to it. Returns NULL if the inotify fd or
 * worker thread couldn't be created. */
FileWatcher *file_watcher_create(const WorkspaceRoot *root);

/* Signals the worker to stop, joins it, and frees everything - safe to
 * call on a NULL watcher. */
void file_watcher_destroy(FileWatcher *watcher);

/* Idempotent - inotify_add_watch() on an already-watched directory just
 * returns the existing watch descriptor, so calling this again on
 * re-expansion is a cheap no-op. Thread-safe to call concurrently with
 * the worker's own poll()/read() loop. A failure (e.g. the directory
 * vanished) is silently ignored - nothing downstream needs to react
 * specially to a directory that can't be watched. */
void file_watcher_watch_directory(FileWatcher *watcher, const char *absolute_directory);

/* Thread-safe; intended for the GUI thread to drain each tick. Never
 * blocks. Returns 0 and leaves *out untouched if no event is pending,
 * 1 if an event was popped into *out. */
int file_watcher_try_pop_event(FileWatcher *watcher, FileWatchEvent *out);

#endif /* WORKBENCH_FILE_WATCHER_H */
