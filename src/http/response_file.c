#include <miniweb/http/response_internal.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
read_entire_file(int fd, char *buf, size_t len)
{
	size_t total;
	ssize_t n;

	total = 0;
	while (total < len) {
		n = read(fd, buf + total, len - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		total += (size_t)n;
	}

	return (total > 0) ? (int)total : -1;
}

int
http_send_file(http_request_t *req, const char *path, const char *mime)
{
	char *body;
	char *cached;
	char buf[65536];
	http_response_t *resp;
	int fd;
	int rc;
	size_t cached_len;
	ssize_t n;
	struct stat st;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return http_send_error(req, 404, "File not found");
	if (fstat(fd, &st) != 0) {
		close(fd);
		return http_send_error(req, 500, "Cannot stat file");
	}

	resp = http_response_create();
	if (!resp) {
		close(fd);
		return -1;
	}
	resp->content_type = mime;
	if (mime && strncmp(mime, "text/plain", 10) == 0)
		http_response_add_header(resp, "Content-Disposition", "inline");

	cached = NULL;
	cached_len = 0;
	if (http_file_cache_lookup(path, &st, &cached, &cached_len)) {
		http_response_set_body(resp, cached, cached_len, 1);
		rc = http_response_send(req, resp);
		http_response_free(resp);
		close(fd);
		return rc;
	}

	if (st.st_size > 0 && (size_t)st.st_size <= FILE_CACHE_MAX_BYTES * 4) {
		body = malloc((size_t)st.st_size);
		if (!body) {
			http_response_free(resp);
			close(fd);
			return -1;
		}
		rc = read_entire_file(fd, body, (size_t)st.st_size);
		close(fd);
		if (rc < 0) {
			free(body);
			http_response_free(resp);
			return http_send_error(req, 500, "Read error");
		}

		http_response_set_body(resp, body, (size_t)rc, 1);
		rc = http_response_send(req, resp);
		if (rc == 0)
			http_file_cache_store(path, &st, body, (size_t)rc);
		http_response_free(resp);
		return rc;
	}

	req->keep_alive = 0;
	http_response_set_body(resp, NULL, (size_t)st.st_size, 0);
	if (http_response_send(req, resp) < 0) {
		http_response_free(resp);
		close(fd);
		return -1;
	}
	http_response_free(resp);

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		if (http_response_write_all(req->fd, buf, (size_t)n) < 0) {
			close(fd);
			return -1;
		}
	}
	close(fd);
	return 0;
}
