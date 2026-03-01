#include <miniweb/core/log.h>
#include <miniweb/http/utils.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

	pid_t pid = fork();
	if (pid == -1) {
		log_debug("[UTILS] fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return NULL;
	}

	if (pid == 0) {
		close(pipefd[0]);

		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(pipefd[1]);

		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}

		execv(path, argv);
		_exit(127);
	}

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
			if (errno == EINTR)
				continue;
			break;
		}

		if (pfd.revents & POLLIN) {
			ssize_t n = read(pipefd[0], buffer + total, max_size - total);
			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					continue;
				break;
			}
			if (n == 0)
				break;
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

char *
safe_popen_read(const char *cmd, size_t max_size)
{
	char *const argv[] = { "sh", "-c", (char *)cmd, NULL };
	return safe_popen_read_argv("/bin/sh", argv, max_size, 5, NULL);
}
