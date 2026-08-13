#include "file_watcher.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "file_watch_queue.h"

#define MAX_WATCH_DIRS 256
#define MAX_PENDING_MOVES 64

typedef struct WatchEntry {
    int wd;
    char absolute_dir[4096];
} WatchEntry;

struct FileWatcher {
    const WorkspaceRoot *root;
    FileWatchQueue *queue;
    int inotify_fd;
    int stop_pipe_read_fd;
    int stop_pipe_write_fd;
    pthread_t thread;

    /* Guards watches/watch_count - written from the GUI thread via
     * file_watcher_watch_directory(), read from the worker thread while
     * translating a raw event's wd back to the directory it belongs
     * to. */
    pthread_mutex_t watch_lock;
    WatchEntry watches[MAX_WATCH_DIRS];
    int watch_count;
};

typedef struct PendingMove {
    uint32_t cookie;
    char relative_path[4096];
    bool used;
} PendingMove;

/* absolute_dir is always root->canonical_path or a subdirectory of it
 * (the only thing ever passed to file_watcher_watch_directory()) - strips
 * that prefix and appends name, producing the same root-relative shape
 * every other path in this subsystem uses. */
static void relative_path_for(const WorkspaceRoot *root, const char *absolute_dir, const char *name, char *out,
                               size_t out_size) {
    size_t root_len = strlen(root->canonical_path);
    const char *rest = absolute_dir + root_len;
    if (*rest == '/') {
        rest++;
    }
    if (*rest == '\0') {
        snprintf(out, out_size, "%s", name);
    } else {
        snprintf(out, out_size, "%s/%s", rest, name);
    }
}

/* Copies the watched directory for wd into dir_out, or returns false if
 * wd is unknown (e.g. IN_Q_OVERFLOW's wd == -1, or a watch already
 * removed) - the caller just skips such an event. */
static bool find_watch_dir(FileWatcher *watcher, int wd, char *dir_out, size_t dir_out_size) {
    bool found = false;
    pthread_mutex_lock(&watcher->watch_lock);
    for (int i = 0; i < watcher->watch_count; i++) {
        if (watcher->watches[i].wd == wd) {
            snprintf(dir_out, dir_out_size, "%s", watcher->watches[i].absolute_dir);
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&watcher->watch_lock);
    return found;
}

/* Processes one full read() buffer of raw inotify events. A rename
 * within the same watched scope reports IN_MOVED_FROM immediately
 * followed by IN_MOVED_TO sharing the same cookie in the same buffer -
 * IN_MOVED_FROM is buffered here and paired against a later IN_MOVED_TO
 * within this same batch; any IN_MOVED_FROM left unpaired once the whole
 * batch is scanned (moved outside any watched directory) degrades to
 * FILE_WATCH_DELETED, and an unpaired IN_MOVED_TO (moved in from
 * somewhere unwatched) is FILE_WATCH_CREATED. */
static void process_batch(FileWatcher *watcher, const char *buf, size_t len) {
    PendingMove pending[MAX_PENDING_MOVES];
    int pending_count = 0;

    size_t offset = 0;
    while (offset < len) {
        const struct inotify_event *ev = (const struct inotify_event *)(buf + offset);
        offset += sizeof(struct inotify_event) + ev->len;

        if (ev->len == 0 || (ev->mask & IN_IGNORED)) {
            continue;
        }

        char dir[4096];
        if (!find_watch_dir(watcher, ev->wd, dir, sizeof(dir))) {
            continue;
        }

        char relative[4096];
        relative_path_for(watcher->root, dir, ev->name, relative, sizeof(relative));

        if (ev->mask & IN_MOVED_FROM) {
            if (pending_count < MAX_PENDING_MOVES) {
                pending[pending_count].cookie = ev->cookie;
                snprintf(pending[pending_count].relative_path, sizeof(pending[pending_count].relative_path), "%s",
                         relative);
                pending[pending_count].used = false;
                pending_count++;
            }
            continue;
        }

        if (ev->mask & IN_MOVED_TO) {
            bool matched = false;
            for (int p = 0; p < pending_count; p++) {
                if (!pending[p].used && pending[p].cookie == ev->cookie) {
                    pending[p].used = true;
                    matched = true;
                    FileWatchEvent out = {0};
                    out.type = FILE_WATCH_RENAMED;
                    snprintf(out.old_relative_path, sizeof(out.old_relative_path), "%s", pending[p].relative_path);
                    snprintf(out.new_relative_path, sizeof(out.new_relative_path), "%s", relative);
                    file_watch_queue_push(watcher->queue, out);
                    break;
                }
            }
            if (!matched) {
                FileWatchEvent out = {0};
                out.type = FILE_WATCH_CREATED;
                snprintf(out.new_relative_path, sizeof(out.new_relative_path), "%s", relative);
                file_watch_queue_push(watcher->queue, out);
            }
            continue;
        }

        if (ev->mask & IN_CREATE) {
            FileWatchEvent out = {0};
            out.type = FILE_WATCH_CREATED;
            snprintf(out.new_relative_path, sizeof(out.new_relative_path), "%s", relative);
            file_watch_queue_push(watcher->queue, out);
            continue;
        }

        if (ev->mask & IN_CLOSE_WRITE) {
            FileWatchEvent out = {0};
            out.type = FILE_WATCH_MODIFIED;
            snprintf(out.new_relative_path, sizeof(out.new_relative_path), "%s", relative);
            file_watch_queue_push(watcher->queue, out);
            continue;
        }

        if (ev->mask & IN_DELETE) {
            FileWatchEvent out = {0};
            out.type = FILE_WATCH_DELETED;
            snprintf(out.new_relative_path, sizeof(out.new_relative_path), "%s", relative);
            file_watch_queue_push(watcher->queue, out);
            continue;
        }
    }

    for (int p = 0; p < pending_count; p++) {
        if (!pending[p].used) {
            FileWatchEvent out = {0};
            out.type = FILE_WATCH_DELETED;
            snprintf(out.new_relative_path, sizeof(out.new_relative_path), "%s", pending[p].relative_path);
            file_watch_queue_push(watcher->queue, out);
        }
    }
}

static void *file_watcher_main(void *arg) {
    FileWatcher *watcher = arg;

    struct pollfd fds[2];
    fds[0].fd = watcher->inotify_fd;
    fds[0].events = POLLIN;
    fds[1].fd = watcher->stop_pipe_read_fd;
    fds[1].events = POLLIN;

    /* inotify_event's variable-length name[] requires the read buffer to
     * be aligned for struct inotify_event itself - the standard idiom
     * from `man 7 inotify`. */
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    for (;;) {
        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (fds[1].revents & POLLIN) {
            break; /* stop requested */
        }
        if (!(fds[0].revents & POLLIN)) {
            continue;
        }

        ssize_t len = read(watcher->inotify_fd, buf, sizeof(buf));
        if (len <= 0) {
            continue;
        }
        process_batch(watcher, buf, (size_t)len);
    }

    return NULL;
}

FileWatcher *file_watcher_create(const WorkspaceRoot *root) {
    int inotify_fd = inotify_init1(0);
    if (inotify_fd < 0) {
        return NULL;
    }

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        close(inotify_fd);
        return NULL;
    }

    FileWatcher *watcher = malloc(sizeof(FileWatcher));
    watcher->root = root;
    watcher->queue = file_watch_queue_create();
    watcher->inotify_fd = inotify_fd;
    watcher->stop_pipe_read_fd = pipe_fds[0];
    watcher->stop_pipe_write_fd = pipe_fds[1];
    pthread_mutex_init(&watcher->watch_lock, NULL);
    watcher->watch_count = 0;

    if (pthread_create(&watcher->thread, NULL, file_watcher_main, watcher) != 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        close(inotify_fd);
        pthread_mutex_destroy(&watcher->watch_lock);
        file_watch_queue_destroy(watcher->queue);
        free(watcher);
        return NULL;
    }

    return watcher;
}

void file_watcher_destroy(FileWatcher *watcher) {
    if (!watcher) {
        return;
    }

    char byte = 1;
    ssize_t ignored = write(watcher->stop_pipe_write_fd, &byte, 1);
    (void)ignored;
    pthread_join(watcher->thread, NULL);

    close(watcher->stop_pipe_write_fd);
    close(watcher->stop_pipe_read_fd);
    close(watcher->inotify_fd);
    pthread_mutex_destroy(&watcher->watch_lock);
    file_watch_queue_destroy(watcher->queue);
    free(watcher);
}

void file_watcher_watch_directory(FileWatcher *watcher, const char *absolute_directory) {
    int wd = inotify_add_watch(watcher->inotify_fd, absolute_directory,
                                IN_CREATE | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if (wd < 0) {
        return;
    }

    pthread_mutex_lock(&watcher->watch_lock);
    bool known = false;
    for (int i = 0; i < watcher->watch_count; i++) {
        if (watcher->watches[i].wd == wd) {
            known = true;
            break;
        }
    }
    if (!known && watcher->watch_count < MAX_WATCH_DIRS) {
        watcher->watches[watcher->watch_count].wd = wd;
        snprintf(watcher->watches[watcher->watch_count].absolute_dir,
                 sizeof(watcher->watches[watcher->watch_count].absolute_dir), "%s", absolute_directory);
        watcher->watch_count++;
    }
    pthread_mutex_unlock(&watcher->watch_lock);
}

int file_watcher_try_pop_event(FileWatcher *watcher, FileWatchEvent *out) {
    return file_watch_queue_try_pop(watcher->queue, out);
}
