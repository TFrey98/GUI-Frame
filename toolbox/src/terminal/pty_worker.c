#define _GNU_SOURCE /* ptsname_r (reentrant; ptsname() alone is not thread-safe) */

#include "pty_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

#define PTY_WORKER_CHUNK_SIZE 4096

/* Heap-allocated, handed to the worker thread and freed by that same
 * thread right before it returns. outgoing/incoming/stop_requested/
 * exited/exit_status are borrowed - owned by the caller's PtyWorker
 * (which outlives this thread), not by this struct. */
typedef struct WorkerArgs {
    int master_fd;
    pid_t child_pid;
    int wake_pipe_read_fd;
    ByteBuffer *outgoing;
    ByteBuffer *incoming;
    atomic_bool *stop_requested;
    atomic_bool *exited;
    atomic_int *exit_status;
} WorkerArgs;

/* Runs in the forked child, never returns on success. working_directory
 * NULL/"" means inherit the parent's cwd unchanged; envv NULL means
 * inherit the parent's environment unchanged - both match
 * terminal_run_command's existing conventions from the VTE-owned spawn
 * path this replaces. */
static void exec_child(const char *slave_path, const char *executable, char *const argv[], char *const envv[],
                        const char *working_directory) {
    setsid();

    int slave_fd = open(slave_path, O_RDWR);
    if (slave_fd < 0) {
        _exit(127);
    }
    /* Establishes the slave as this new session's controlling terminal -
     * required for shell job control (Ctrl+C/Ctrl+Z reaching the right
     * process group) to work at all. */
    ioctl(slave_fd, TIOCSCTTY, 0);

    dup2(slave_fd, STDIN_FILENO);
    dup2(slave_fd, STDOUT_FILENO);
    dup2(slave_fd, STDERR_FILENO);
    if (slave_fd > STDERR_FILENO) {
        close(slave_fd);
    }

    if (working_directory && *working_directory && chdir(working_directory) != 0) {
        _exit(127);
    }

    execve(executable, argv, envv ? envv : environ);
    _exit(127); /* only reached if execve() itself failed */
}

static void *pty_worker_main(void *arg) {
    WorkerArgs *args = arg;

    unsigned char read_buf[PTY_WORKER_CHUNK_SIZE];
    unsigned char write_buf[PTY_WORKER_CHUNK_SIZE];

    for (;;) {
        struct pollfd fds[2];
        fds[0].fd = args->master_fd;
        /* POLLOUT only when there's actually something queued to send -
         * same reasoning as connection_worker.c: an always-writable pty
         * would otherwise busy-loop poll(). The wake pipe re-evaluates
         * this the moment new outgoing data shows up. */
        fds[0].events = (short)(POLLIN | (byte_buffer_len(args->outgoing) > 0 ? POLLOUT : 0));
        fds[1].fd = args->wake_pipe_read_fd;
        fds[1].events = POLLIN;

        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; /* unexpected poll() failure - fall through and report closed */
        }

        if (fds[1].revents & POLLIN) {
            /* Non-blocking read end (set in pty_worker_start) so this
             * drain loop terminates via EAGAIN once the pipe is empty. */
            char discard[16];
            while (read(args->wake_pipe_read_fd, discard, sizeof(discard)) > 0) {
                /* drain every queued wake byte so poll() doesn't immediately refire */
            }
            if (atomic_load(args->stop_requested)) {
                break;
            }
            /* else: just a "new outgoing data" wake - loop back and recompute pollfds */
        }

        if (fds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(args->master_fd, read_buf, sizeof(read_buf));
            if (n > 0) {
                byte_buffer_append(args->incoming, read_buf, (size_t)n);
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                break; /* child exited and closed its end, or a real read error */
            }
        }

        if (fds[0].revents & POLLOUT) {
            /* Peek (not consume) - only discard what write() actually
             * confirmed sending, so a partial write never silently drops
             * the un-sent remainder. */
            size_t n = byte_buffer_peek(args->outgoing, write_buf, sizeof(write_buf));
            if (n > 0) {
                ssize_t written = write(args->master_fd, write_buf, n);
                if (written < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        break; /* real error */
                    }
                } else if (written > 0) {
                    byte_buffer_discard(args->outgoing, (size_t)written);
                }
            }
        }
    }

    close(args->master_fd);
    close(args->wake_pipe_read_fd);

    int status = 0;
    /* The child may not have fully exited the instant the master fd
     * reports EOF (it closes its pty fds as part of exiting, which can
     * race the kernel finishing process teardown by a hair) - a blocking
     * waitpid() here is the same "this thread may block, that's its job"
     * tolerance connection_worker.c already accepts for socket I/O. */
    waitpid(args->child_pid, &status, 0);
    atomic_store(args->exit_status, status);
    atomic_store(args->exited, true);

    free(args);
    return NULL;
}

int pty_worker_start(PtyWorker *out, const char *executable, char *const argv[], char *const envv[],
                      const char *working_directory) {
    /* CLOEXEC on the master fd (belt-and-suspenders: passed to
     * posix_openpt() and re-applied via fcntl() right after, in case this
     * platform's posix_openpt() doesn't honor the flag) matters beyond
     * this one terminal: without it, every *other* terminal's own
     * fork()+exec() (each duplicating this process's whole fd table)
     * would hand its child a stray extra copy of this master fd. That
     * extra copy keeps the underlying pty's open-file-description
     * refcount above zero even after this worker closes its own copy on
     * stop, so the hangup this terminal's own child is waiting for would
     * never actually arrive - the exact deadlock this fixes. */
    int master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master_fd < 0) {
        return -1;
    }
    fcntl(master_fd, F_SETFD, fcntl(master_fd, F_GETFD, 0) | FD_CLOEXEC);
    if (grantpt(master_fd) != 0 || unlockpt(master_fd) != 0) {
        close(master_fd);
        return -1;
    }
    char slave_path[256];
    if (ptsname_r(master_fd, slave_path, sizeof(slave_path)) != 0) {
        close(master_fd);
        return -1;
    }

    /* Same CLOEXEC reasoning as master_fd above, for both pipe ends. */
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
        close(master_fd);
        return -1;
    }
    /* Non-blocking read end: the wake path drains every queued wake byte
     * in a loop, and that loop needs read() to return EAGAIN once the
     * pipe is empty rather than blocking on a byte that isn't coming. */
    fcntl(pipe_fds[0], F_SETFL, fcntl(pipe_fds[0], F_GETFL, 0) | O_NONBLOCK);

    pid_t child_pid = fork();
    if (child_pid < 0) {
        close(master_fd);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    if (child_pid == 0) {
        /* Child: never returns. */
        close(master_fd);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        exec_child(slave_path, executable, argv, envv, working_directory);
    }

    atomic_bool *stop_requested = malloc(sizeof(atomic_bool));
    atomic_init(stop_requested, false);
    atomic_bool *exited = malloc(sizeof(atomic_bool));
    atomic_init(exited, false);
    atomic_int *exit_status = malloc(sizeof(atomic_int));
    atomic_init(exit_status, 0);
    ByteBuffer *outgoing = byte_buffer_create(0);
    ByteBuffer *incoming = byte_buffer_create(0);

    WorkerArgs *args = malloc(sizeof(WorkerArgs));
    args->master_fd = master_fd;
    args->child_pid = child_pid;
    args->wake_pipe_read_fd = pipe_fds[0];
    args->outgoing = outgoing;
    args->incoming = incoming;
    args->stop_requested = stop_requested;
    args->exited = exited;
    args->exit_status = exit_status;

    pthread_t thread;
    if (pthread_create(&thread, NULL, pty_worker_main, args) != 0) {
        close(master_fd);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        byte_buffer_destroy(outgoing);
        byte_buffer_destroy(incoming);
        free(stop_requested);
        free(exited);
        free(exit_status);
        free(args);
        return -1;
    }

    out->thread = thread;
    out->wake_pipe_write_fd = pipe_fds[1];
    out->outgoing = outgoing;
    out->incoming = incoming;
    out->stop_requested = stop_requested;
    out->exited = exited;
    out->exit_status = exit_status;
    out->master_fd = master_fd;
    return 0;
}

void pty_worker_send(PtyWorker *worker, const void *data, size_t len) {
    byte_buffer_append(worker->outgoing, data, len);
    char byte = 1;
    ssize_t ignored = write(worker->wake_pipe_write_fd, &byte, 1);
    (void)ignored;
}

void pty_worker_signal_stop(PtyWorker *worker) {
    atomic_store(worker->stop_requested, true);
    char byte = 1;
    ssize_t ignored = write(worker->wake_pipe_write_fd, &byte, 1);
    (void)ignored;
}

void pty_worker_join(PtyWorker *worker) {
    pthread_join(worker->thread, NULL);
    close(worker->wake_pipe_write_fd);
    byte_buffer_destroy(worker->outgoing);
    byte_buffer_destroy(worker->incoming);
    free(worker->stop_requested);
    free(worker->exited);
    free(worker->exit_status);
}
