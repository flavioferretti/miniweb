#include <miniweb/http/response_internal.h>

#include <errno.h>
#include <poll.h>
#include <unistd.h>

static int
http_response_wait_fd_writable(int fd)
{
	struct pollfd pfd;
	int rc;

	pfd.fd = fd;
	pfd.events = POLLOUT;
	pfd.revents = 0;
	for (;;) {
		rc = poll(&pfd, 1, WRITE_WAIT_MS);
		if (rc > 0) {
			if (pfd.revents & POLLOUT)
				return 0;
			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
				return -1;
			continue;
		}
		if (rc == 0)
			return -1;
		if (errno == EINTR)
			continue;
		return -1;
	}
}

int
http_response_write_all(int fd, const void *buf, size_t n)
{
	const char *p;
	int retries;
	size_t remaining;
	ssize_t w;

	p = buf;
	remaining = n;
	retries = 0;
	while (remaining > 0) {
		w = write(fd, p, remaining);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (retries++ > WRITE_RETRY_LIMIT)
					return -1;
				if (http_response_wait_fd_writable(fd) < 0)
					return -1;
				continue;
			}
			return -1;
		}
		if (w == 0)
			return -1;
		p += w;
		remaining -= (size_t)w;
		retries = 0;
	}
	return 0;
}

int
http_response_writev_all(int fd, struct iovec *iov, int iovcnt)
{
	int idx;
	int retries;
	size_t left;
	ssize_t w;

	idx = 0;
	retries = 0;
	while (idx < iovcnt) {
		w = writev(fd, &iov[idx], iovcnt - idx);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (retries++ > WRITE_RETRY_LIMIT)
					return -1;
				if (http_response_wait_fd_writable(fd) < 0)
					return -1;
				continue;
			}
			return -1;
		}
		if (w == 0)
			return -1;

		retries = 0;
		left = (size_t)w;
		while (idx < iovcnt && left >= iov[idx].iov_len) {
			left -= iov[idx].iov_len;
			idx++;
		}
		if (idx < iovcnt && left > 0) {
			iov[idx].iov_base = (char *)iov[idx].iov_base + left;
			iov[idx].iov_len -= left;
		}
	}

	return 0;
}

const char *
http_response_status_text(int status_code)
{
	switch (status_code) {
	case 200:
		return "OK";
	case 301:
		return "Moved Permanently";
	case 302:
		return "Found";
	case 304:
		return "Not Modified";
	case 400:
		return "Bad Request";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 500:
		return "Internal Server Error";
	case 503:
		return "Service Unavailable";
	default:
		return "Unknown";
	}
}
