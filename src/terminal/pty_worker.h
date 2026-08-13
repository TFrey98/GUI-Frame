#ifndef WORKBENCH_PTY_WORKER_H
#define WORKBENCH_PTY_WORKER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "byte_buffer.h"

/*
 * One reader/writer thread per locally-spawned shell/command, same
 * self-pipe + poll() shape as ConnectionWorker (connection_worker.h) -
 * except the fd is a pty master this worker opened itself (instead of an
 * already-accepted socket), and there's a child process to reap instead
 * of just a fd to close. Reads into `incoming` (GUI thread drains it into
 * a TerminalHistory each tick, feeding the VTE widget for display and
 * persisting to the database); writes whatever's in `outgoing` (GUI
 * thread appends to it via pty_worker_send(), fed by the terminal's own
 * commit signal). Owned directly by a Terminal - unlike connections, a
 * local terminal is always exactly one worker per one view, so there's no
 * registry/manager table needed.
 */
typedef struct PtyWorker {
    pthread_t thread;
    int wake_pipe_write_fd; /* one pipe, dual purpose: new-outgoing-data wake, or stop wake */
    ByteBuffer *outgoing;
    ByteBuffer *incoming;
    atomic_bool *stop_requested; /* heap-allocated, shared with the thread; freed by pty_worker_join */
    atomic_bool *exited;         /* set once, by the worker, right after it reaps the child */
    atomic_int *exit_status;     /* valid once *exited is true; the raw waitpid() status */
    int master_fd; /* set before the thread starts, then read-only; the GUI thread may read it (never close it)
                     * to issue TIOCSWINSZ on resize, safe because that's a plain ioctl concurrent with the
                     * worker thread's own blocking poll()/read()/write() on the same fd - only ever done
                     * between a successful pty_worker_start() and the matching pty_worker_join(). */
} PtyWorker;

/* Opens a pty, forks, and execve()s executable/argv (envv NULL means
 * inherit the current environment unchanged, matching terminal_run_command's
 * existing convention) with working_directory as the child's cwd (NULL/""
 * means inherit unchanged). The child becomes its own session leader with
 * the pty slave as controlling terminal, so shell job control works. Spawns
 * the worker thread immediately. Returns 0 on success (out is filled in),
 * -1 if the pty/pipe/thread couldn't even be created (out is left
 * untouched). A failed execve() inside the child is reported the same way
 * a normal process exit is (status 127) - there is no separate synchronous
 * "spawn failed" signal, since by the time execve() runs the thread is
 * already committed and started. */
int pty_worker_start(PtyWorker *out, const char *executable, char *const argv[], char *const envv[],
                      const char *working_directory);

/* Wakes the worker's poll() without requesting a stop - call after
 * appending new data to worker->outgoing. */
void pty_worker_send(PtyWorker *worker, const void *data, size_t len);

/* Requests a stop and wakes the worker's poll() to notice it. Closing the
 * pty master delivers SIGHUP to the child's foreground process group (the
 * same implicit behavior VTE relied on when it owned the pty), so this is
 * usually enough for the shell to exit on its own; the worker still waits
 * for it via waitpid() before reporting back. Does not block or join. */
void pty_worker_signal_stop(PtyWorker *worker);

/* Joins the thread, then frees its outgoing/incoming buffers and the
 * shared atomics, and closes the wake pipe's write end. Drain
 * worker->incoming yourself first if you still want its contents - this
 * destroys it. Only safe once the worker's *exited flag is observed true
 * (natural exit) or after pty_worker_signal_stop() (forced stop). */
void pty_worker_join(PtyWorker *worker);

#endif /* WORKBENCH_PTY_WORKER_H */
