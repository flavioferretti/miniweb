/* utils.c - Utility functions: JSON escaping, string sanitization,
 *           subprocess execution.
 *
 * FIX #2: safe_popen_read_argv now uses fork() instead of vfork().
 *
 * vfork() shares the parent's address space until execv().  The previous
 * implementation called open("/dev/null", ...) in the child AFTER vfork(),
 * which is legal POSIX-only if the child calls _exit() or execv() without
 * modifying any data visible to the parent — but open() modifies the
 * process's file-descriptor table, which IS shared with the parent under
 * vfork().  With 32+ worker threads all calling vfork() concurrently, the
 * dup2/open sequences in different children clobber each other's (and the
 * parent's) fds, causing stale reads on client sockets, EBADF on writes,
 * and the observed hang/crash under load.
 *
 * fork() copies the address space so the child's fd operations are private.
 * The cost of the extra copy-on-write page-table clone is negligible
 * compared with the mandoc/apropos execution time that follows.
 */

#include <miniweb/http/utils.h>
#include <miniweb/core/log.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Produce a heap-allocated JSON-escaped copy of @p src.
 *
 * Special characters (double-quote, backslash, and ASCII control codes)
 * are escaped with their JSON backslash sequences.  The caller must free
 * the returned string.
 *
 * @param src Source string, or NULL (returns an empty string).
 * @return Newly allocated escaped string, or an empty strdup on failure.
 */
char *
json_escape_string(const char *src)
{
    if (!src)
        return strdup("");

    size_t len = strlen(src);
    /* Worst case: every byte expands to \uXXXX (6 chars) + potential
     * backslash escapes. Use 6x + 1 to be safe. */
    size_t max_out = len * 6 + 1;
    char *dest = malloc(max_out);
    if (!dest)
        return strdup("");

    size_t d = 0;
    for (size_t s = 0; src[s] != '\0'; s++) {
        /* Ensure we have room for longest possible sequence + NUL */
        if (d + 7 >= max_out - 1)
            break;

        switch (src[s]) {
            case '"':  dest[d++] = '\\'; dest[d++] = '"';  break;
            case '\\': dest[d++] = '\\'; dest[d++] = '\\'; break;
            case '\b': dest[d++] = '\\'; dest[d++] = 'b';  break;
            case '\f': dest[d++] = '\\'; dest[d++] = 'f';  break;
            case '\n': dest[d++] = '\\'; dest[d++] = 'n';  break;
            case '\r': dest[d++] = '\\'; dest[d++] = 'r';  break;
            case '\t': dest[d++] = '\\'; dest[d++] = 't';  break;
            default:
                if ((unsigned char)src[s] < 0x20) {
                    d += snprintf(dest + d, max_out - d, "\\u%04x",
                                  (unsigned char)src[s]);
                } else {
                    dest[d++] = src[s];
                }
                break;
        }
    }
    dest[d] = '\0';
    return dest;
}//bugfix - Buffer Overrun in json_escape_string

/**
 * @brief Replace characters that are unsafe for filesystem paths with '_'.
 *
 * Permitted characters: alphanumeric, '.', '-', '_', '+'.
 * All other bytes are overwritten in-place.
 *
 * @param s NUL-terminated string to sanitise. Modified in-place.
 */
void
sanitize_string(char *s)
{
    if (!s)
        return;
    while (*s) {
        if (!isalnum((unsigned char)*s) &&
            *s != '.' && *s != '-' && *s != '_' && *s != '+')
            *s = '_';
        s++;
    }
}

/**
 * @brief Execute @p path with @p argv, capture stdout, and return it as a
 *        heap buffer.
 *
 * Uses fork() (not vfork()) so that child-side fd operations — dup2,
 * open("/dev/null") — do not race with concurrent parent threads that share
 * the same file-descriptor table under vfork().
 *
 * @param path            Absolute path to the executable.
 * @param argv            NULL-terminated argument vector (argv[0] = program name).
 * @param max_size        Maximum bytes to read from stdout.
 * @param timeout_seconds Hard deadline; child is SIGKILL'd on expiry.
 * @param out_len         Optional: receives the number of bytes read.
 * @return Heap-allocated NUL-terminated output, or NULL on timeout/error.
 *         Caller must free().
 */
char *
safe_popen_read_argv(const char *path, char *const argv[],
                     size_t max_size, int timeout_seconds, size_t *out_len)
{
    int pipefd[2];

    log_debug("[UTILS] Executing: %s", path);

    if (pipe(pipefd) == -1) {
        log_debug("[UTILS] pipe() failed: %s", strerror(errno));
        return NULL;
    }

    /* FIX #2: fork() instead of vfork() — child fd mutations are private */
    pid_t pid = fork();
    if (pid == -1) {
        log_debug("[UTILS] fork() failed: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    if (pid == 0) {
        /* Child process — address space is a private copy after fork(). */
        close(pipefd[0]);

        /* Redirect stdout to pipe write-end. */
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(pipefd[1]);

        /* Redirect stderr to /dev/null — safe here because we have our
         * own copy of the fd table. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        execv(path, argv);
        _exit(127);  /* execv() failed */
    }

    /* Parent */
    close(pipefd[1]);

    char *buffer = malloc(max_size + 1);
    if (!buffer) {
        log_debug("[UTILS] malloc(%zu) failed", max_size + 1);
        close(pipefd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return NULL;
    }

    size_t total    = 0;
    int    timed_out = 0;
    time_t deadline  = time(NULL) + (timeout_seconds > 0 ? timeout_seconds : 5);

    while (total < max_size) {
        time_t now = time(NULL);
        if (now >= deadline) {
            timed_out = 1;
            break;
        }

        int wait_ms = (int)((deadline - now) * 1000);
        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        int pr = poll(&pfd, 1, wait_ms > 0 ? wait_ms : 0);

        if (pr == 0) { timed_out = 1; break; }
        if (pr < 0)  { if (errno == EINTR) continue; break; }

        if (pfd.revents & POLLIN) {
            ssize_t n = read(pipefd[0], buffer + total, max_size - total);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;
            }
            if (n == 0) break;
            total += (size_t)n;
        }
        if (pfd.revents & (POLLERR | POLLHUP))
            break;
    }

    close(pipefd[0]);

    if (timed_out)
        kill(pid, SIGKILL);

    int status;
    waitpid(pid, &status, 0);

    if (out_len)
        *out_len = total;

    if (timed_out || total == 0) {
        free(buffer);
        return NULL;
    }

    buffer[total] = '\0';
    return buffer;
}

/**
 * @brief Run a shell command and return its stdout as a heap buffer.
 *
 * Convenience wrapper around safe_popen_read_argv() that executes @p cmd
 * via /bin/sh -c with a fixed 5-second timeout.
 *
 * @param cmd      Shell command string.
 * @param max_size Maximum bytes to read from stdout.
 * @return Heap-allocated NUL-terminated output, or NULL on timeout/error.
 */
char *
safe_popen_read(const char *cmd, size_t max_size)
{
    char *const argv[] = { "sh", "-c", (char *)cmd, NULL };
    return safe_popen_read_argv("/bin/sh", argv, max_size, 5, NULL);
}
