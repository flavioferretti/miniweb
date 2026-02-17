/* http_utils.c - Utility functions: JSON escaping, string sanitization,
 *                subprocess execution.
 *
 * No libmicrohttpd dependency. */

#include "http_utils.h"
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* JSON string escaping — caller must free() */
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

/* Replace characters unsafe for filesystem use with '_'.
 * Permits: alphanumeric, '.', '-', '_', '+' */
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

/* Execute path with argv, capture up to max_size bytes of output.
 * Enforces a wall-clock timeout (seconds).  Returns malloc'd buffer
 * NUL-terminated, or NULL on error / timeout / empty output.
 * Caller must free(). */
char *
safe_popen_read_argv(const char *path, char *const argv[],
					 size_t max_size, int timeout_seconds)
{
	int pipefd[2];
	if (pipe(pipefd) == -1)
		return NULL;

	pid_t pid = fork();
	if (pid == -1) {
		close(pipefd[0]);
		close(pipefd[1]);
		return NULL;
	}

	if (pid == 0) {
		/* Child */
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execv(path, argv);
		_exit(127);
	}

	/* Parent */
	close(pipefd[1]);

	size_t capacity = max_size > 0 ? max_size : 1;
	char *buffer = malloc(capacity + 1);
	if (!buffer) {
		close(pipefd[0]);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return NULL;
	}

	size_t total    = 0;
	int    timed_out = 0;
	time_t deadline  = time(NULL) +
	(timeout_seconds > 0 ? timeout_seconds : 5);

	while (total < max_size) {
		time_t now = time(NULL);
		if (now >= deadline) {
			timed_out = 1;
			break;
		}
		int wait_ms = (int)((deadline - now) * 1000);
		struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
		int pr = poll(&pfd, 1, wait_ms);
		if (pr == 0) { timed_out = 1; break; }
		if (pr < 0)  { if (errno == EINTR) continue; break; }

		if (pfd.revents & POLLIN) {
			ssize_t n = read(pipefd[0], buffer + total,
							 max_size - total);
			if (n <= 0) break;
			total += (size_t)n;
		}
		if (pfd.revents & (POLLERR | POLLHUP))
			break;
	}

	close(pipefd[0]);

	if (timed_out)
		kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);

	buffer[total] = '\0';

	if (timed_out || total == 0) {
		free(buffer);
		return NULL;
	}
	return buffer;
}

/* Convenience wrapper: run cmd through /bin/sh -c */
char *
safe_popen_read(const char *cmd, size_t max_size)
{
	char *const argv[] = { "sh", "-c", (char *)cmd, NULL };
	return safe_popen_read_argv("/bin/sh", argv, max_size, 5);
}
