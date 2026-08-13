/*
 * Headless coverage for PtyWorker (src/terminal/pty_worker.c), the piece
 * that replaced vte_terminal_spawn_async's built-in pty ownership so local
 * shell output/input can be captured. Same polling style as
 * connection_io_test.c's pump()-based waits, since there's no event queue
 * here to drain - just the worker's own atomics/ByteBuffer.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

#include "listeners/byte_buffer.h"
#include "terminal/pty_worker.h"

static const char *TEST_NAME = "pty_worker_test";

static bool wait_for_incoming_containing(PtyWorker *worker, const char *needle, int timeout_ms) {
    static char acc[4096];
    size_t acc_len = 0;
    int waited = 0;
    while (waited < timeout_ms) {
        unsigned char chunk[256];
        size_t n;
        while (acc_len + 1 < sizeof(acc) &&
               (n = byte_buffer_consume(worker->incoming, chunk, sizeof(chunk))) > 0) {
            size_t copy = n < sizeof(acc) - acc_len - 1 ? n : sizeof(acc) - acc_len - 1;
            memcpy(acc + acc_len, chunk, copy);
            acc_len += copy;
        }
        acc[acc_len] = '\0';
        if (strstr(acc, needle) != NULL) {
            return true;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
        nanosleep(&ts, NULL);
        waited += 5;
    }
    return false;
}

static bool wait_for_exit(PtyWorker *worker, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        if (atomic_load(worker->exited)) {
            return true;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
        nanosleep(&ts, NULL);
        waited += 5;
    }
    return false;
}

int main(void) {
    int status = 0;

    /* --- typed input round-trips back through incoming (kernel pty echo), and a
     * forced stop reaps the child without hanging --- */
    {
        PtyWorker worker;
        char *argv[] = {"/bin/cat", NULL};
        if (pty_worker_start(&worker, "/bin/cat", argv, NULL, NULL) != 0) {
            fprintf(stderr, "%s: pty_worker_start failed\n", TEST_NAME);
            return 1;
        }

        static const char input[] = "hello-from-test\n";
        pty_worker_send(&worker, input, strlen(input));
        if (!wait_for_incoming_containing(&worker, "hello-from-test", 2000)) {
            fprintf(stderr, "%s: expected echoed input never arrived\n", TEST_NAME);
            status = 1;
        }

        pty_worker_signal_stop(&worker);
        pty_worker_join(&worker); /* must return promptly, not hang */
    }

    /* --- a command that exits on its own reports the real exit status --- */
    {
        PtyWorker worker;
        char *argv[] = {"/bin/sh", "-c", "exit 7", NULL};
        if (pty_worker_start(&worker, "/bin/sh", argv, NULL, NULL) != 0) {
            fprintf(stderr, "%s: pty_worker_start failed for exit-status case\n", TEST_NAME);
            return 1;
        }

        if (!wait_for_exit(&worker, 2000)) {
            fprintf(stderr, "%s: worker never reported the child's exit\n", TEST_NAME);
            status = 1;
        } else {
            int raw_status = atomic_load(worker.exit_status);
            if (!WIFEXITED(raw_status) || WEXITSTATUS(raw_status) != 7) {
                fprintf(stderr, "%s: expected exit status 7, got raw waitpid() status %d\n", TEST_NAME, raw_status);
                status = 1;
            }
        }

        pty_worker_join(&worker); /* thread already returned on its own - must not hang */
    }

    /* --- a sibling worker's fds must not keep an unrelated worker's child
     * alive after stop (the CLOEXEC regression pty_worker.c's comment
     * documents: without it, this pair would deadlock in waitpid()) --- */
    {
        PtyWorker a, b;
        char *argv_a[] = {"/bin/cat", NULL};
        char *argv_b[] = {"/bin/cat", NULL};
        if (pty_worker_start(&a, "/bin/cat", argv_a, NULL, NULL) != 0 ||
            pty_worker_start(&b, "/bin/cat", argv_b, NULL, NULL) != 0) {
            fprintf(stderr, "%s: pty_worker_start failed for cross-worker cloexec case\n", TEST_NAME);
            return 1;
        }

        pty_worker_signal_stop(&a);
        pty_worker_join(&a);
        pty_worker_signal_stop(&b);
        pty_worker_join(&b);
    }

    if (status == 0) {
        printf("%s: echo round-trip, forced stop, natural exit status, and cross-worker fd isolation all "
               "verified\n",
               TEST_NAME);
    }
    return status;
}
