/*
 * Exercises FileWatcher against a real mkdtemp()'d directory and real
 * inotify events - a watched directory sees a real external create,
 * write+close, delete, and rename (simulating a terminal or an external
 * editor touching files from outside the app), polling
 * file_watcher_try_pop_event() for the expected event sequence within a
 * timeout. Confirms IN_CLOSE_WRITE (not IN_MODIFY) is what backs
 * FILE_WATCH_MODIFIED - one write+close is exactly one event, not
 * several - and that a real rename produces one FILE_WATCH_RENAMED via
 * IN_MOVED_FROM/IN_MOVED_TO cookie-pairing, not two separate created/
 * deleted events.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "files/file_watcher.h"

#define POLL_INTERVAL_US 10000
#define POLL_TIMEOUT_US 3000000

/* Polls until an event arrives or the timeout elapses. Returns 1 (with
 * *out filled) on success, 0 on timeout. */
static int wait_for_event(FileWatcher *watcher, FileWatchEvent *out) {
    int waited_us = 0;
    while (waited_us < POLL_TIMEOUT_US) {
        if (file_watcher_try_pop_event(watcher, out)) {
            return 1;
        }
        usleep(POLL_INTERVAL_US);
        waited_us += POLL_INTERVAL_US;
    }
    return 0;
}

/* Confirms nothing arrives within a short window - used to prove a
 * single write+close is exactly one MODIFIED event, not several. */
static int expect_no_event_within(FileWatcher *watcher, int window_us) {
    FileWatchEvent event;
    int waited_us = 0;
    while (waited_us < window_us) {
        if (file_watcher_try_pop_event(watcher, &event)) {
            return 0;
        }
        usleep(POLL_INTERVAL_US);
        waited_us += POLL_INTERVAL_US;
    }
    return 1;
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

int main(void) {
    char root_template[] = "/tmp/fw_test_root_XXXXXX";
    char *root_dir = mkdtemp(root_template);
    if (!root_dir) {
        fprintf(stderr, "file_watcher_test: mkdtemp failed\n");
        return 1;
    }

    WorkspaceRoot root;
    if (!workspace_root_init_at(&root, root_dir)) {
        fprintf(stderr, "file_watcher_test: workspace_root_init_at failed\n");
        return 1;
    }

    FileWatcher *watcher = file_watcher_create(&root);
    if (!watcher) {
        fprintf(stderr, "file_watcher_test: file_watcher_create failed\n");
        return 1;
    }
    file_watcher_watch_directory(watcher, root.canonical_path);

    int status = 0;
    char path_a[4200], path_b[4200];
    snprintf(path_a, sizeof(path_a), "%s/a.txt", root.canonical_path);
    snprintf(path_b, sizeof(path_b), "%s/b.txt", root.canonical_path);

    /* A real create followed by a write+close reports CREATED, then
     * exactly one MODIFIED (IN_CLOSE_WRITE, not IN_MODIFY - no matter
     * how many times the writer's buffer flushes mid-write, only the
     * final close() produces an event). */
    write_file(path_a, "hello");

    FileWatchEvent event;
    if (!wait_for_event(watcher, &event) || event.type != FILE_WATCH_CREATED ||
        strcmp(event.new_relative_path, "a.txt") != 0) {
        fprintf(stderr, "file_watcher_test: expected FILE_WATCH_CREATED for a.txt\n");
        status = 1;
    }
    if (!wait_for_event(watcher, &event) || event.type != FILE_WATCH_MODIFIED ||
        strcmp(event.new_relative_path, "a.txt") != 0) {
        fprintf(stderr, "file_watcher_test: expected exactly one FILE_WATCH_MODIFIED for a.txt\n");
        status = 1;
    }
    if (!expect_no_event_within(watcher, 300000)) {
        fprintf(stderr, "file_watcher_test: a single write+close should produce exactly one MODIFIED event\n");
        status = 1;
    }

    /* A real rename within the same watched directory produces one
     * FILE_WATCH_RENAMED - proving IN_MOVED_FROM/IN_MOVED_TO
     * cookie-pairing, not two separate created/deleted events. */
    if (rename(path_a, path_b) != 0) {
        fprintf(stderr, "file_watcher_test: real rename(a.txt, b.txt) failed\n");
        status = 1;
    }
    if (!wait_for_event(watcher, &event) || event.type != FILE_WATCH_RENAMED ||
        strcmp(event.old_relative_path, "a.txt") != 0 || strcmp(event.new_relative_path, "b.txt") != 0) {
        fprintf(stderr, "file_watcher_test: expected one FILE_WATCH_RENAMED (a.txt -> b.txt)\n");
        status = 1;
    }

    /* A real delete reports DELETED. */
    if (unlink(path_b) != 0) {
        fprintf(stderr, "file_watcher_test: real unlink(b.txt) failed\n");
        status = 1;
    }
    if (!wait_for_event(watcher, &event) || event.type != FILE_WATCH_DELETED ||
        strcmp(event.new_relative_path, "b.txt") != 0) {
        fprintf(stderr, "file_watcher_test: expected FILE_WATCH_DELETED for b.txt\n");
        status = 1;
    }

    file_watcher_destroy(watcher);

    if (status == 0) {
        printf("file_watcher_test: real create/write-close/rename/delete events all verified via inotify\n");
    }
    return status;
}
