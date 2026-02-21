/* http_handler.c - HTTP handler utilities */

#include "../include/config.h"
#include "http_handler.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define WRITE_RETRY_LIMIT 256
#define WRITE_WAIT_MS 100
#define FILE_CACHE_SLOTS 32
#define FILE_CACHE_MAX_BYTES (256 * 1024)
#define FILE_CACHE_INSERTS_PER_SEC 8
#define FILE_CACHE_MAX_AGE_SEC 120

typedef struct {
	http_response_t items[256];
	int free_stack[256];
	int free_top;
	pthread_mutex_t lock;
	int initialized;
} response_pool_t;

static response_pool_t response_pool = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

static int
wait_fd_writable(int fd)
{
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLOUT;
	pfd.revents = 0;

	for (;;) {
		int rc = poll(&pfd, 1, WRITE_WAIT_MS);
		if (rc > 0)
			return 0;
		if (rc == 0)
			return -1;
		if (errno == EINTR)
			continue;
		return -1;
	}
}

typedef struct file_cache_entry {
	char path[512];
	char *data;
	size_t len;
	time_t mtime;
	time_t atime;
} file_cache_entry_t;

typedef struct file_cache_candidate {
	char path[512];
	unsigned int hits;
	time_t atime;
} file_cache_candidate_t;

static file_cache_entry_t file_cache[FILE_CACHE_SLOTS];
static file_cache_candidate_t file_cache_candidates[FILE_CACHE_SLOTS * 2];
static pthread_mutex_t file_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static int cache_insert_tokens = FILE_CACHE_INSERTS_PER_SEC;
static time_t cache_insert_window = 0;

static void
response_pool_init_locked(void)
{
	if (response_pool.initialized)
		return;

	response_pool.free_top = 256;
	for (int i = 0; i < 256; i++)
		response_pool.free_stack[i] = 255 - i;
	response_pool.initialized = 1;
}

static void
file_cache_refill_budget_locked(time_t now)
{
	if (cache_insert_window == 0 || now != cache_insert_window) {
		cache_insert_window = now;
		cache_insert_tokens = FILE_CACHE_INSERTS_PER_SEC;
	}
}

static void
file_cache_evict_stale_locked(time_t now)
{
	for (int i = 0; i < FILE_CACHE_SLOTS; i++) {
		if (file_cache[i].path[0] == '\0')
			continue;
		if ((now - file_cache[i].atime) > FILE_CACHE_MAX_AGE_SEC) {
			free(file_cache[i].data);
			memset(&file_cache[i], 0, sizeof(file_cache[i]));
		}
	}

	for (int i = 0; i < FILE_CACHE_SLOTS * 2; i++) {
		if (file_cache_candidates[i].path[0] == '\0')
			continue;
		if ((now - file_cache_candidates[i].atime) > FILE_CACHE_MAX_AGE_SEC)
			memset(&file_cache_candidates[i], 0,
			       sizeof(file_cache_candidates[i]));
	}
}

static int
file_cache_admit_locked(const char *path, time_t now)
{
	int slot = -1;
	time_t oldest = 0;

	for (int i = 0; i < FILE_CACHE_SLOTS * 2; i++) {
		if (file_cache_candidates[i].path[0] == '\0') {
			slot = i;
			break;
		}
		if (strcmp(file_cache_candidates[i].path, path) == 0) {
			file_cache_candidates[i].hits++;
			file_cache_candidates[i].atime = now;
			return file_cache_candidates[i].hits >= 2;
		}
		if (slot == -1 || file_cache_candidates[i].atime < oldest) {
			oldest = file_cache_candidates[i].atime;
			slot = i;
		}
	}

	if (slot < 0)
		return 0;

	strlcpy(file_cache_candidates[slot].path, path,
		sizeof(file_cache_candidates[slot].path));
	file_cache_candidates[slot].hits = 1;
	file_cache_candidates[slot].atime = now;
	return 0;
}

static void
file_cache_store(const char *path, const struct stat *st, const char *data,
		 size_t len)
{
	if (!path || !st || !data || len == 0 || len > FILE_CACHE_MAX_BYTES)
		return;

	pthread_mutex_lock(&file_cache_lock);
	time_t now = time(NULL);
	file_cache_refill_budget_locked(now);
	file_cache_evict_stale_locked(now);

	if (cache_insert_tokens <= 0) {
		pthread_mutex_unlock(&file_cache_lock);
		return;
	}

	if (!file_cache_admit_locked(path, now)) {
		pthread_mutex_unlock(&file_cache_lock);
		return;
	}

	int slot = -1;
	time_t oldest = 0;

	for (int i = 0; i < FILE_CACHE_SLOTS; i++) {
		if (file_cache[i].path[0] == '\0') {
			slot = i;
			break;
		}
		if (slot == -1 || file_cache[i].atime < oldest) {
			oldest = file_cache[i].atime;
			slot = i;
		}
	}

	if (slot >= 0) {
		cache_insert_tokens--;
		free(file_cache[slot].data);
		file_cache[slot].data = malloc(len);
		if (file_cache[slot].data) {
			memcpy(file_cache[slot].data, data, len);
			strlcpy(file_cache[slot].path, path,
				sizeof(file_cache[slot].path));
			file_cache[slot].len = len;
			file_cache[slot].mtime = st->st_mtime;
			file_cache[slot].atime = now;
		} else {
			memset(&file_cache[slot], 0, sizeof(file_cache[slot]));
		}
	}

	pthread_mutex_unlock(&file_cache_lock);
}

static int
file_cache_lookup(const char *path, const struct stat *st, char **out,
		  size_t *out_len)
{
	int found = 0;

	if (!path || !st || !out || !out_len || st->st_size <= 0 ||
	    (size_t)st->st_size > FILE_CACHE_MAX_BYTES)
		return 0;

	pthread_mutex_lock(&file_cache_lock);
	time_t now = time(NULL);
	file_cache_refill_budget_locked(now);
	file_cache_evict_stale_locked(now);

	for (int i = 0; i < FILE_CACHE_SLOTS; i++) {
		if (file_cache[i].path[0] == '\0')
			continue;
		if (strcmp(file_cache[i].path, path) == 0 &&
		    file_cache[i].mtime == st->st_mtime) {
			*out = malloc(file_cache[i].len);
			if (*out) {
				memcpy(*out, file_cache[i].data, file_cache[i].len);
				*out_len = file_cache[i].len;
				file_cache[i].atime = now;
				found = 1;
			}
			break;
		}
	}
	pthread_mutex_unlock(&file_cache_lock);

	return found;
}

/* Create response */
http_response_t *
http_response_create(void)
{
	pthread_mutex_lock(&response_pool.lock);
	response_pool_init_locked();

	http_response_t *resp = NULL;
	if (response_pool.free_top > 0) {
		int idx = response_pool.free_stack[--response_pool.free_top];
		resp = &response_pool.items[idx];
		memset(resp, 0, sizeof(*resp));
	}
	pthread_mutex_unlock(&response_pool.lock);

	if (!resp)
		return NULL;

	resp->status_code = 200;
	resp->content_type = "text/html; charset=utf-8";
	return resp;
}

/* Set status code */
void
http_response_set_status(http_response_t *resp, int code)
{
	resp->status_code = code;
}

/* Set response body */
void
http_response_set_body(http_response_t *resp, char *body, size_t len,
		       int must_free)
{
	resp->body = body;
	resp->body_len = len; /* CRITICAL: use exact length, not strlen() */
	resp->free_body = must_free;
}

/* Add response header */
void
http_response_add_header(http_response_t *resp, const char *name,
			 const char *value)
{
	int len = snprintf(resp->headers + resp->headers_len,
			   sizeof(resp->headers) - resp->headers_len,
			   "%s: %s\r\n", name, value);
	if (len > 0) {
		resp->headers_len += len;
	}
}

/* Write exactly n bytes, retrying on partial writes.
 * Returns 0 on success, -1 on error or closed connection. */
/* Write exactly n bytes, retrying on partial writes and EAGAIN.
 * Returns 0 on success, -1 on error or closed connection. */
static int
write_all(int fd, const void *buf, size_t n)
{
	const char *p = buf;
	size_t remaining = n;
	int retries = 0;

	while (remaining > 0) {
		ssize_t w = write(fd, p, remaining);
		if (w < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (retries++ > WRITE_RETRY_LIMIT) {
					return -1;
				}
				if (wait_fd_writable(fd) < 0)
					return -1;
				continue;
			}
			return -1;
		}
		if (w == 0) {
			return -1;
		}
		p += w;
		remaining -= (size_t)w;
		retries = 0; /* Reset retries after a successful write. */
	}
	return 0;
}

static int
writev_all(int fd, struct iovec *iov, int iovcnt)
{
	int idx = 0;
	int retries = 0;

	while (idx < iovcnt) {
		ssize_t w = writev(fd, &iov[idx], iovcnt - idx);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (retries++ > WRITE_RETRY_LIMIT)
					return -1;
				if (wait_fd_writable(fd) < 0)
					return -1;
				continue;
			}
			return -1;
		}
		if (w == 0)
			return -1;

		retries = 0;
		size_t left = (size_t)w;
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

/* Send HTTP response */
/* Temporary debug note for http_response_send. */
int http_response_send(http_request_t *req, http_response_t *resp)
{
	char header[4096];
	int header_len;

	const char *status_text;
	switch (resp->status_code) {
		case 200: status_text = "OK"; break;
		case 400: status_text = "Bad Request"; break;
		case 403: status_text = "Forbidden"; break;
		case 404: status_text = "Not Found"; break;
		case 500: status_text = "Internal Server Error"; break;
		case 503: status_text = "Service Unavailable"; break;
		default:  status_text = "Unknown"; break;
	}

	/* Build HTTP header */
	header_len = snprintf(header, sizeof(header),
						  "HTTP/1.1 %d %s\r\n"
						  "Content-Type: %s\r\n"
						  "Content-Length: %zu\r\n"
					  "Connection: %s\r\n"
					  "Server: MiniWeb/kqueue\r\n",
				   resp->status_code, status_text,
				   resp->content_type,
				   resp->body_len,
				   req->keep_alive ? "keep-alive" : "close");

	/* Append custom headers if they fit */
	if (resp->headers_len > 0) {
		int space = (int)sizeof(header) - header_len;
		if ((int)resp->headers_len < space) {
			memcpy(header + header_len, resp->headers, resp->headers_len);
			header_len += (int)resp->headers_len;
		}
	}

	/* Terminate header section */
	header_len += snprintf(header + header_len,
						   sizeof(header) - header_len, "\r\n");

	//fprintf(stderr, "[HTTP] Sending response: status=%d, type=%s, length=%zu\n",
	//		resp->status_code, resp->content_type, resp->body_len);

	struct iovec iov[2];
	iov[0].iov_base = header;
	iov[0].iov_len = (size_t)header_len;
	iov[1].iov_base = resp->body;
	iov[1].iov_len = resp->body_len;

	if (resp->body && resp->body_len > 0) {
		if (writev_all(req->fd, iov, 2) < 0) {
			fprintf(stderr, "[HTTP] Error writing response\n");
			return -1;
		}
	} else if (write_all(req->fd, header, (size_t)header_len) < 0) {
		fprintf(stderr, "[HTTP] Error writing headers\n");
		return -1;
	}

	return 0;
}

/* Free response */
void
http_response_free(http_response_t *resp)
{
	if (!resp)
		return;

	/* If free_body is 1, release the allocated body buffer (for example
	  * from fread). */
	if (resp->free_body && resp->body) {
		free(resp->body);
	}

	ptrdiff_t idx = resp - response_pool.items;
	if (idx >= 0 && idx < 256) {
		pthread_mutex_lock(&response_pool.lock);
		memset(resp, 0, sizeof(*resp));
		if (response_pool.free_top < 256)
			response_pool.free_stack[response_pool.free_top++] = (int)idx;
		pthread_mutex_unlock(&response_pool.lock);
		return;
	}

	free(resp);
}

/* Get request header - writes into caller-supplied buffer, thread-safe */
const char *
http_request_get_header(http_request_t *req, const char *name)
{
	/* Use per-request scratch space inside http_request_t.
	 * Callers must NOT store the returned pointer beyond the
	 * lifetime of the request — it points into req->hdr_scratch. */
	char search[256];
	snprintf(search, sizeof(search), "\r\n%s:", name);

	const char *header = strcasestr(req->buffer, search);
	if (!header) {
		/* Try matching at the very first line (no leading \r\n) */
		snprintf(search, sizeof(search), "%s:", name);
		header = strcasestr(req->buffer, search);
		if (!header)
			return NULL;
	}

	const char *colon = strchr(header, ':');
	if (!colon)
		return NULL;

	const char *val = colon + 1;
	while (*val == ' ' || *val == '\t')
		val++;

	const char *end = strstr(val, "\r\n");
	if (!end)
		return NULL;

	size_t len = end - val;
	if (len >= sizeof(req->hdr_scratch))
		len = sizeof(req->hdr_scratch) - 1;

	memcpy(req->hdr_scratch, val, len);
	req->hdr_scratch[len] = '\0';

	return req->hdr_scratch;
}

/* Get real client IP - writes into req->ip_scratch, thread-safe */
const char *
http_request_get_client_ip(http_request_t *req)
{
	/* X-Real-IP */
	const char *real_ip = http_request_get_header(req, "X-Real-IP");
	if (real_ip && real_ip[0]) {
		/* already stored in req->hdr_scratch; copy to ip_scratch
		 * so it survives a subsequent get_header call */
		strlcpy(req->ip_scratch, real_ip, sizeof(req->ip_scratch));
		return req->ip_scratch;
	}

	/* X-Forwarded-For: take the first entry */
	const char *forwarded = http_request_get_header(req, "X-Forwarded-For");
	if (forwarded && forwarded[0]) {
		const char *comma = strchr(forwarded, ',');
		size_t len =
		    comma ? (size_t)(comma - forwarded) : strlen(forwarded);
		if (len >= sizeof(req->ip_scratch))
			len = sizeof(req->ip_scratch) - 1;
		memcpy(req->ip_scratch, forwarded, len);
		req->ip_scratch[len] = '\0';
		return req->ip_scratch;
	}

	/* Fall back to socket peer address */
	inet_ntop(AF_INET, &req->client_addr->sin_addr, req->ip_scratch,
		  sizeof(req->ip_scratch));
	return req->ip_scratch;
}

/* Check if the request arrived over HTTPS (via reverse-proxy header) */
int
http_request_is_https(http_request_t *req)
{
	const char *proto = http_request_get_header(req, "X-Forwarded-Proto");
	return (proto && strcmp(proto, "https") == 0);
}

/* Send error response */
int
http_send_error(http_request_t *req, int status_code, const char *message)
{
	char body[2048];
	int body_len;

	body_len = snprintf(
	    body, sizeof(body),
	    "<!DOCTYPE html><html><head>"
	    "<meta charset=\"UTF-8\">"
	    "<title>%d Error</title>"
	    "<link rel=\"stylesheet\" href=\"/static/css/custom.css\">"
	    "</head><body>"
		"<div class=\"container\">"
	    "<h1>%d Error</h1>"
	    "<p>%s</p>"
	    "<hr><p><a href=\"/\">MiniWeb</a> on OpenBSD</p></div>"
	    "</body></html>",
	    status_code, status_code, message ? message : "An error occurred");

	http_response_t *resp = http_response_create();
	http_response_set_status(resp, status_code);
	http_response_set_body(resp, body, body_len, 0);

	int ret = http_response_send(req, resp);
	http_response_free(resp);
	return ret;
}

/* Send JSON response */
int
http_send_json(http_request_t *req, const char *json)
{
	http_response_t *resp = http_response_create();
	if (!resp)
		return -1;

	resp->content_type = "application/json";
	http_response_set_body(resp, (char *)json, strlen(json), 0);

	int ret = http_response_send(req, resp);
	http_response_free(resp);
	return ret;
}

/* Send HTML response */
int
http_send_html(http_request_t *req, const char *html)
{
	http_response_t *resp = http_response_create();
	if (!resp)
		return -1;

	http_response_set_body(resp, (char *)html, strlen(html), 0);

	int ret = http_response_send(req, resp);
	http_response_free(resp);
	return ret;
}

/* Versione corretta di http_send_file */
int
http_send_file(http_request_t *req, const char *path, const char *mime)
{
	// printf("DEBUG: http_send_file opening: %s\n", path);

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		// printf("DEBUG: Failed to open file: %s (errno=%d)\n", path,
		// errno);
		return http_send_error(req, 404, "File not found");
	}

	struct stat st;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return http_send_error(req, 500, "Cannot stat file");
	}

	http_response_t *resp = http_response_create();
	if (!resp) {
		close(fd);
		return -1;
	}
	resp->content_type = mime;

	char *cached = NULL;
	size_t cached_len = 0;
	if (file_cache_lookup(path, &st, &cached, &cached_len)) {
		http_response_set_body(resp, cached, cached_len, 1);
		int rc = http_response_send(req, resp);
		http_response_free(resp);
		close(fd);
		return rc;
	}

	http_response_set_body(resp, NULL, (size_t)st.st_size, 0);
	if (http_response_send(req, resp) < 0) {
		http_response_free(resp);
		close(fd);
		return -1;
	}
	http_response_free(resp);

	char file_buf[65536];
	char *cache_copy = NULL;
	size_t cache_used = 0;
	if ((size_t)st.st_size > 0 && (size_t)st.st_size <= FILE_CACHE_MAX_BYTES)
		cache_copy = malloc((size_t)st.st_size);

	ssize_t n;
	while ((n = read(fd, file_buf, sizeof(file_buf))) > 0) {
		if (cache_copy && cache_used + (size_t)n <= (size_t)st.st_size) {
			memcpy(cache_copy + cache_used, file_buf, (size_t)n);
			cache_used += (size_t)n;
		}
		if (write_all(req->fd, file_buf, (size_t)n) < 0) {
			free(cache_copy);
			close(fd);
			return -1;
		}
	}

	if (cache_copy && cache_used == (size_t)st.st_size)
		file_cache_store(path, &st, cache_copy, cache_used);
	free(cache_copy);

	close(fd);
	// printf("DEBUG: Successfully sent file: %s (%lld bytes)\n",
	//	   path, (long long)st.st_size);
	return 0;
}

/* In http_handler.c */
int
http_render_template(http_request_t *req, struct template_data *data,
		     const char *fallback_template)
{
	char *output = NULL;

	/* Try rendering with structured template data first. */
	if (template_render_with_data(data, &output) != 0) {
		/* If that fails and page content exists, try basic rendering. */
		if (data->page_content &&
		    template_render(data->page_content, &output) != 0) {
			/* If all rendering paths fail, use fallback or return 500. */
			return http_send_error(
			    req, 500,
			    fallback_template ? fallback_template
				      : "Template rendering failed");
		}
	}

	http_response_t *resp = http_response_create();
	if (!resp) {
		free(output);
		return -1;
	}

	/* Parameter '1' tells http_response_set_body to free 'output'. */
	http_response_set_body(resp, output, strlen(output), 1);

	int ret = http_response_send(req, resp);
	http_response_free(resp);

	return ret;
}
