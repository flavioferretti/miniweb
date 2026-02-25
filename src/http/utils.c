
/* utils.c - Utility functions: JSON escaping, string sanitization,
 *                subprocess execution.
 *
 * No libmicrohttpd dependency. */

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

/* JSON string escaping — caller must free() */
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
/* See json_escape_string() Doxygen block above. */
char *
json_escape_string(const char *src)
{
	if (!src)
		return strdup("");

	size_t len = strlen(src);
	/* Worst case: every character becomes two ("\\x") */
	char *dest = malloc(len * 2 + 1);
	if (!dest)
		return strdup("");

	size_t d = 0;
	for (size_t s = 0; src[s] != '\0' && d < len * 2; s++) {
		switch (src[s]) {
			case '"':  dest[d++] = '\\'; dest[d++] = '"';  break;
			case '\\': dest[d++] = '\\'; dest[d++] = '\\'; break;
			case '\b': dest[d++] = '\\'; dest[d++] = 'b';  break;
			case '\f': dest[d++] = '\\'; dest[d++] = 'f';  break;
			case '\n': dest[d++] = '\\'; dest[d++] = 'n';  break;
			case '\r': dest[d++] = '\\'; dest[d++] = 'r';  break;
			case '\t': dest[d++] = '\\'; dest[d++] = 't';  break;
			default:   dest[d++] = src[s];                  break;
		}
	}
	dest[d] = '\0';
	return dest;
}

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

char *
safe_popen_read_argv(const char *path, char *const argv[],
					 size_t max_size, int timeout_seconds, size_t *out_len)
{
	int pipefd[2];

	log_debug("[UTILS] Attempting to execute: %s", path);
	for (int i = 0; argv[i] != NULL; i++) {
		log_debug("[UTILS]   argv[%d]: '%s'", i, argv[i]);
	}

	if (pipe(pipefd) == -1) {
		log_debug("[UTILS] pipe() failed: %s", strerror(errno));
		return NULL;
	}

	/* Use vfork() instead of fork() — safer when pledge(2) is active. */
	pid_t pid = vfork();
	if (pid == -1) {
		log_debug("[UTILS] vfork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return NULL;
	}

	if (pid == 0) {
		/* Child process — shares address space with parent until execv(). */
		close(pipefd[0]);

		/* Redirect stdout to pipe */
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);

		/* Redirect stderr to /dev/null */
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}

		/* Execute the command. */
		execv(path, argv);

		/* If we reach here, execv() failed. */
		_exit(127);
	}

	/* Parent process */
	close(pipefd[1]);

	char *buffer = malloc(max_size + 1);
	if (!buffer) {
		log_debug("[UTILS] malloc(%zu) failed", max_size + 1);
		close(pipefd[0]);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return NULL;
	}

	size_t total = 0;
	int timed_out = 0;
	time_t deadline = time(NULL) + (timeout_seconds > 0 ? timeout_seconds : 5);

	while (total < max_size) {
		time_t now = time(NULL);
		if (now >= deadline) {
			timed_out = 1;
			break;
		}

		int wait_ms = (int)((deadline - now) * 1000);
		struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
		int pr = poll(&pfd, 1, wait_ms > 0 ? wait_ms : 0);

		if (pr == 0) {
			timed_out = 1;
			break;
		}
		if (pr < 0) {
			if (errno == EINTR) continue;
			break;
		}

		if (pfd.revents & POLLIN) {
			ssize_t n = read(pipefd[0], buffer + total, max_size - total);
			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
				break;
			}
			if (n == 0) break;
			total += (size_t)n;
		}
		if (pfd.revents & (POLLERR | POLLHUP)) break;
	}

	close(pipefd[0]);

	if (timed_out) {
		kill(pid, SIGKILL);
	}

	int status;
	waitpid(pid, &status, 0);

	if (out_len) {
		*out_len = total;
	}

	if (timed_out || total == 0) {
		free(buffer);
		return NULL;
	}

	buffer[total] = '\0';
	return buffer;
}

/* Convenience wrapper: run cmd through /bin/sh -c */
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
/* See safe_popen_read() Doxygen block above. */
char *
safe_popen_read(const char *cmd, size_t max_size)
{
	char *const argv[] = { "sh", "-c", (char *)cmd, NULL };
	return safe_popen_read_argv("/bin/sh", argv, max_size, 5, NULL);
}
