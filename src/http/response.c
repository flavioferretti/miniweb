/* response.c - HTTP handler utilities */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <miniweb/core/config.h>
#include <miniweb/http/handler.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <miniweb/core/log.h>
#include <miniweb/router/urls.h>

#define WRITE_RETRY_LIMIT 5
#define WRITE_WAIT_MS 50
#define FILE_CACHE_SLOTS 32
#define FILE_CACHE_MAX_BYTES (256 * 1024)
#define FILE_CACHE_INSERTS_PER_SEC 8
#define FILE_CACHE_MAX_AGE_SEC 240
#define RESPONSE_POOL_SHARDS 16
#define FILE_CACHE_SHARDS 16

/**
 * @brief Internal data structure.
 */
typedef struct {
	http_response_t items[1024];
	int free_stack[1024];
	int free_top;
	pthread_mutex_t lock;
	int initialized;
} response_pool_t;

static response_pool_t response_pools[RESPONSE_POOL_SHARDS];

/**
 * @brief Wait fd writable.
 * @param fd File descriptor to operate on.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
static int
wait_fd_writable(int fd)
{
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLOUT;
	pfd.revents = 0;

	for (;;) {
		int rc = poll(&pfd, 1, WRITE_WAIT_MS);
		if (rc > 0) {
			if (pfd.revents & POLLOUT)
				return 0;
			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
				return -1;
			continue;
		}

		/* Timeout: return error to avoid busy-waiting */
		if (rc == 0)
			return -1;   /* timeout — socket not draining, give up */

		if (errno == EINTR)
			continue;

		return -1;
	}
}// bugfix - wait_fd_writable Spin-on-Timeout

/**
 * @brief Internal data structure.
 */
typedef struct file_cache_entry {
	char path[512];
	char *data;
	size_t len;
	time_t mtime;
	time_t atime;
} file_cache_entry_t;

/**
 * @brief Internal data structure.
 */
typedef struct file_cache_candidate {
	char path[512];
	unsigned int hits;
	time_t atime;
} file_cache_candidate_t;

typedef struct {
	file_cache_entry_t entries[FILE_CACHE_SLOTS];
	file_cache_candidate_t candidates[FILE_CACHE_SLOTS * 2];
	pthread_mutex_t lock;
	int cache_insert_tokens;
	time_t cache_insert_window;
	unsigned int window_hits;
	unsigned int window_misses;
	unsigned int window_inserts;
	unsigned int window_throttles;
} file_cache_shard_t;

static file_cache_shard_t file_cache_shards[FILE_CACHE_SHARDS];
static pthread_once_t cache_once = PTHREAD_ONCE_INIT;

static void
http_handler_globals_init(void)
{
	for (int i = 0; i < RESPONSE_POOL_SHARDS; i++)
		pthread_mutex_init(&response_pools[i].lock, NULL);

	for (int i = 0; i < FILE_CACHE_SHARDS; i++) {
		pthread_mutex_init(&file_cache_shards[i].lock, NULL);
		file_cache_shards[i].cache_insert_tokens =
		    FILE_CACHE_INSERTS_PER_SEC;
	}
}

static inline unsigned long
thread_hash(void)
{
	uintptr_t tid = (uintptr_t)pthread_self();
	return (unsigned long)(tid ^ (tid >> 7) ^ (tid >> 13));
}

static int
response_pool_shard_index(void)
{
	return (int)(thread_hash() % RESPONSE_POOL_SHARDS);
}

static int
file_cache_shard_index(const char *path)
{
	unsigned long hash = 1469598103934665603UL;

	for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
		hash ^= *p;
		hash *= 1099511628211UL;
	}

	return (int)(hash % FILE_CACHE_SHARDS);
}

/**
 * @brief Response pool init locked.
 */
static void
response_pool_init_locked(response_pool_t *pool)
{
	if (pool->initialized)
		return;

	pool->free_top = 1024;
	for (int i = 0; i < 1024; i++)
		pool->free_stack[i] = 1023 - i;
	pool->initialized = 1;
}

/**
 * @brief File cache refill budget locked.
 * @param now Parameter used by this function.
 */
static void
file_cache_refill_budget_locked(file_cache_shard_t *shard, time_t now,
				int shard_idx)
{
	if (shard->cache_insert_window == 0 ||
	    now != shard->cache_insert_window) {
		if (shard->cache_insert_window != 0) {
			log_debug("[FILE_CACHE] shard=%d/sec=%ld hits=%u "
				  "misses=%u inserts=%u throttles=%u budget=%d",
				  shard_idx, (long)shard->cache_insert_window,
				  shard->window_hits, shard->window_misses,
				  shard->window_inserts,
				  shard->window_throttles,
				  FILE_CACHE_INSERTS_PER_SEC);
		}
		shard->cache_insert_window = now;
		shard->cache_insert_tokens = FILE_CACHE_INSERTS_PER_SEC;
		shard->window_hits = 0;
		shard->window_misses = 0;
		shard->window_inserts = 0;
		shard->window_throttles = 0;
	}
}

/**
 * @brief File cache evict stale locked.
 * @param now Parameter used by this function.
 */
static void
file_cache_evict_stale_locked(file_cache_shard_t *shard, time_t now)
{
	for (int i = 0; i < FILE_CACHE_SLOTS; i++) {
		if (shard->entries[i].path[0] == '\0')
			continue;
		if ((now - shard->entries[i].atime) > FILE_CACHE_MAX_AGE_SEC) {
			free(shard->entries[i].data);
			memset(&shard->entries[i], 0,
			       sizeof(shard->entries[i]));
		}
	}

	for (int i = 0; i < FILE_CACHE_SLOTS * 2; i++) {
		if (shard->candidates[i].path[0] == '\0')
			continue;
		if ((now - shard->candidates[i].atime) > FILE_CACHE_MAX_AGE_SEC)
			memset(&shard->candidates[i], 0,
			       sizeof(shard->candidates[i]));
	}
}

/**
 * @brief File cache admit locked.
 * @param path Request or filesystem path to evaluate.
 * @param now Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
static int
file_cache_admit_locked(file_cache_shard_t *shard, const char *path, time_t now)
{
	int slot = -1;
	time_t oldest = 0;

	for (int i = 0; i < FILE_CACHE_SLOTS * 2; i++) {
		if (shard->candidates[i].path[0] == '\0') {
			slot = i;
			break;
		}
		if (strcmp(shard->candidates[i].path, path) == 0) {
			shard->candidates[i].hits++;
			shard->candidates[i].atime = now;
			return shard->candidates[i].hits >= 2;
		}
		if (slot == -1 || shard->candidates[i].atime < oldest) {
			oldest = shard->candidates[i].atime;
			slot = i;
		}
	}

	if (slot < 0)
		return 0;

	strlcpy(shard->candidates[slot].path, path,
		sizeof(shard->candidates[slot].path));
	shard->candidates[slot].hits = 1;
	shard->candidates[slot].atime = now;
	return 0;
}

/**
 * @brief Store a static file payload in the in-memory cache.
 * @param path Cache key path.
 * @param st File metadata used for validation.
 * @param data File contents.
 * @param len File size in bytes.
 */
static void
file_cache_store(const char *path, const struct stat *st, const char *data,
		 size_t len)
{
	if (!path || !st || !data || len == 0 || len > FILE_CACHE_MAX_BYTES)
		return;
	pthread_once(&cache_once, http_handler_globals_init);
	int shard_idx = file_cache_shard_index(path);
	file_cache_shard_t *shard = &file_cache_shards[shard_idx];

	pthread_mutex_lock(&shard->lock);
	time_t now = time(NULL);
	file_cache_refill_budget_locked(shard, now, shard_idx);
	file_cache_evict_stale_locked(shard, now);

	if (shard->cache_insert_tokens <= 0) {
		shard->window_throttles++;
		pthread_mutex_unlock(&shard->lock);
		return;
	}

	if (!file_cache_admit_locked(shard, path, now)) {
		pthread_mutex_unlock(&shard->lock);
		return;
	}

	int slot = -1;
	time_t oldest = 0;

	for (int i = 0; i < FILE_CACHE_SLOTS; i++) {
		if (shard->entries[i].path[0] == '\0') {
			slot = i;
			break;
		}
		if (slot == -1 || shard->entries[i].atime < oldest) {
			oldest = shard->entries[i].atime;
			slot = i;
		}
	}

	if (slot >= 0) {
		shard->cache_insert_tokens--;
		free(shard->entries[slot].data);
		shard->entries[slot].data = malloc(len);
		if (shard->entries[slot].data) {
			memcpy(shard->entries[slot].data, data, len);
			strlcpy(shard->entries[slot].path, path,
				sizeof(shard->entries[slot].path));
			shard->entries[slot].len = len;
			shard->entries[slot].mtime = st->st_mtime;
			shard->entries[slot].atime = now;
			shard->window_inserts++;
		} else {
			memset(&shard->entries[slot], 0,
			       sizeof(shard->entries[slot]));
		}
	}

	pthread_mutex_unlock(&shard->lock);
}

/**
 * @brief Lookup and duplicate a cached static file payload.
 * @param path Cache key path.
 * @param st Current file metadata.
 * @param out Receives duplicated payload on hit.
 * @param out_len Receives payload length on hit.
 * @return 1 if cache hit, 0 otherwise.
 */
static int
file_cache_lookup(const char *path, const struct stat *st, char **out,
		  size_t *out_len)
{
	int found = 0;

	if (!path || !st || !out || !out_len || st->st_size <= 0 ||
	    (size_t)st->st_size > FILE_CACHE_MAX_BYTES)
		return 0;
	pthread_once(&cache_once, http_handler_globals_init);
	int shard_idx = file_cache_shard_index(path);
	file_cache_shard_t *shard = &file_cache_shards[shard_idx];

	pthread_mutex_lock(&shard->lock);
	time_t now = time(NULL);
	file_cache_refill_budget_locked(shard, now, shard_idx);
	file_cache_evict_stale_locked(shard, now);

	for (int i = 0; i < FILE_CACHE_SLOTS; i++) {
		if (shard->entries[i].path[0] == '\0')
			continue;
		if (strcmp(shard->entries[i].path, path) == 0 &&
		    shard->entries[i].mtime == st->st_mtime) {
			*out = malloc(shard->entries[i].len);
			if (*out) {
				memcpy(*out, shard->entries[i].data,
				       shard->entries[i].len);
				*out_len = shard->entries[i].len;
				shard->entries[i].atime = now;
				shard->window_hits++;
				found = 1;
			}
			break;
		}
	}
	if (!found)
		shard->window_misses++;
	pthread_mutex_unlock(&shard->lock);

	return found;
}

/* Create response */
/**
 * @brief Http response create.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
http_response_t *
http_response_create(void)
{
	pthread_once(&cache_once, http_handler_globals_init);
	int shard_idx = response_pool_shard_index();
	response_pool_t *pool = &response_pools[shard_idx];

	pthread_mutex_lock(&pool->lock);
	response_pool_init_locked(pool);

	http_response_t *resp = NULL;
	if (pool->free_top > 0) {
		int idx = pool->free_stack[--pool->free_top];
		resp = &pool->items[idx];
		memset(resp, 0, sizeof(*resp));
	}
	pthread_mutex_unlock(&pool->lock);

	if (!resp) {
		resp = calloc(1, sizeof(*resp));
		if (!resp)
			return NULL;
		resp->pool_shard_idx = -1;
	} else {
		resp->pool_shard_idx = shard_idx;
	}

	resp->status_code = 200;
	resp->content_type = "text/html; charset=utf-8";
	return resp;
}

/* Set status code */
/**
 * @brief Http response set status.
 * @param resp Response object to emit or mutate.
 * @param code HTTP status code to send.
 */
void
http_response_set_status(http_response_t *resp, int code)
{
	resp->status_code = code;
}

/* Set response body */
/**
 * @brief Set body pointer and ownership metadata for a response.
 * @param resp Response object to mutate.
 * @param body Response body pointer.
 * @param len Response body size in bytes.
 * @param must_free Non-zero if body must be freed with the response.
 */
void
http_response_set_body(http_response_t *resp, char *body, size_t len,
		       int must_free)
{
	resp->body = body;
	resp->body_len = len; /* CRITICAL: use exact length, not strlen() */
	resp->free_body = must_free;
}

/* Add response header */
/**
 * @brief Append a header line to a response object.
 * @param resp Response object to mutate.
 * @param name Header name.
 * @param value Header value.
 */
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
/**
 * @brief Write all.
 * @param fd File descriptor to operate on.
 * @param buf Input buffer containing textual data.
 * @param n Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
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

/**
 * @brief Writev all.
 * @param fd File descriptor to operate on.
 * @param iov Parameter used by this function.
 * @param iovcnt Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
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
/**
 * @brief http_response_send.
 */
int
http_response_send(http_request_t *req, http_response_t *resp)
{
	char header[4096];
	int header_len;

	const char *status_text;
	switch (resp->status_code) {
	case 200:
		status_text = "OK";
		break;
	case 301:
		status_text = "Moved Permanently";
		break;
	case 302:
		status_text = "Found";
		break;
	case 304:
		status_text = "Not Modified";
		break;
	case 400:
		status_text = "Bad Request";
		break;
	case 403:
		status_text = "Forbidden";
		break;
	case 404:
		status_text = "Not Found";
		break;
	case 405:
		status_text = "Method Not Allowed";
		break;
	case 500:
		status_text = "Internal Server Error";
		break;
	case 503:
		status_text = "Service Unavailable";
		break;
	default:
		status_text = "Unknown";
		break;
	} // bugfix - Missing case 405 in Status Switch

	/* Build HTTP header */
	header_len =
	    snprintf(header, sizeof(header),
		     "HTTP/1.1 %d %s\r\n"
		     "Content-Type: %s\r\n"
		     "Content-Length: %zu\r\n"
		     "Connection: %s\r\n"
		     "Server: MiniWeb/kqueue\r\n",
		     resp->status_code, status_text, resp->content_type,
		     resp->body_len, req->keep_alive ? "keep-alive" : "close");

	/* Append custom headers if they fit */
	if (resp->headers_len > 0) {
		int space = (int)sizeof(header) - header_len;
		if ((int)resp->headers_len < space) {
			memcpy(header + header_len, resp->headers,
			       resp->headers_len);
			header_len += (int)resp->headers_len;
		}
	}

	/* Terminate header section */
	header_len +=
	    snprintf(header + header_len, sizeof(header) - header_len, "\r\n");

	// fprintf(stderr, "[HTTP] Sending response: status=%d, type=%s,
	// length=%zu\n", 		resp->status_code, resp->content_type,
	//resp->body_len);

	struct iovec iov[2];
	iov[0].iov_base = header;
	iov[0].iov_len = (size_t)header_len;
	iov[1].iov_base = resp->body;
	iov[1].iov_len = resp->body_len;

	if (resp->body && resp->body_len > 0) {
		if (writev_all(req->fd, iov, 2) < 0) {
			log_error("[HTTP] Error writing response");
			return -1;
		}
	} else if (write_all(req->fd, header, (size_t)header_len) < 0) {
		log_error("[HTTP] Error writing headers");
		return -1;
	}

	return 0;
}

/* Free response */
/**
 * @brief Http response free.
 * @param resp Response object to emit or mutate.
 */
void
http_response_free(http_response_t *resp)
{
	if (!resp)
		return;
	pthread_once(&cache_once, http_handler_globals_init);

	/* If free_body is 1, release the allocated body buffer (for example
	 * from fread). */
	if (resp->free_body && resp->body) {
		free(resp->body);
	}

	if (resp->pool_shard_idx >= 0 &&
	    resp->pool_shard_idx < RESPONSE_POOL_SHARDS) {
		response_pool_t *pool = &response_pools[resp->pool_shard_idx];
		if (resp >= pool->items && resp < (pool->items + 1024)) {
			ptrdiff_t idx = resp - pool->items;
			pthread_mutex_lock(&pool->lock);
			memset(resp, 0, sizeof(*resp));
			if (pool->free_top < 1024)
				pool->free_stack[pool->free_top++] = (int)idx;
			pthread_mutex_unlock(&pool->lock);
			return;
		}
	}

	for (int shard_idx = 0; shard_idx < RESPONSE_POOL_SHARDS; shard_idx++) {
		response_pool_t *pool = &response_pools[shard_idx];
		if (resp >= pool->items && resp < (pool->items + 1024)) {
			ptrdiff_t idx = resp - pool->items;
			pthread_mutex_lock(&pool->lock);
			memset(resp, 0, sizeof(*resp));
			if (pool->free_top < 1024)
				pool->free_stack[pool->free_top++] = (int)idx;
			pthread_mutex_unlock(&pool->lock);
			return;
		}
	}

	free(resp);
}

/* Get request header - writes into caller-supplied buffer, thread-safe */
/**
 * @brief Http request get header.
 * @param req Request context for response generation.
 * @param name Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
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
/**
 * @brief Http request get client ip.
 * @param req Request context for response generation.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
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
/**
 * @brief Http request is https.
 * @param req Request context for response generation.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
int
http_request_is_https(http_request_t *req)
{
	const char *proto = http_request_get_header(req, "X-Forwarded-Proto");
	return (proto && strcmp(proto, "https") == 0);
}

/* Send error response */
/**
 * @brief Http send error.
 * @param req Request context for response generation.
 * @param status_code Parameter used by this function.
 * @param message Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
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
	if (!resp)
		return -1;
	http_response_set_status(resp, status_code);
	http_response_set_body(resp, body, body_len, 0);

	if (status_code == 405 && req && req->url) {
		char allow[256];
		if (route_allow_methods(req->url, allow, sizeof(allow)) > 0)
			http_response_add_header(resp, "Allow", allow);
	}

	int ret = http_response_send(req, resp);
	http_response_free(resp);
	return ret;
}

/* Send JSON response */
/**
 * @brief Build and send a 200 application/json response.
 * @param req  Incoming request context.
 * @param json NUL-terminated JSON string to send.
 * @return 0 on success, -1 on write failure.
 */
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
/**
 * @brief Build and send a 200 text/html response.
 * @param req  Incoming request context.
 * @param html NUL-terminated HTML string to send.
 * @return 0 on success, -1 on write failure.
 */
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

/* Correct implementation of http_send_file with cache support. */
/**
 * @brief Serve a local file as an HTTP response with MIME type @p mime.
 *
 * Consults the sharded static file cache before falling back to direct
 * read.  Returns 404 when the file cannot be opened.
 *
 * @param req  Incoming request context.
 * @param path Absolute filesystem path of the file to serve.
 * @param mime Content-Type header value.
 * @return 0 on success, -1 on write failure.
 */
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
	if (mime && strncmp(mime, "text/plain", 10) == 0)
		http_response_add_header(resp, "Content-Disposition", "inline");

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
	if ((size_t)st.st_size > 0 &&
	    (size_t)st.st_size <= FILE_CACHE_MAX_BYTES)
		cache_copy = malloc((size_t)st.st_size);

	ssize_t n;
	while ((n = read(fd, file_buf, sizeof(file_buf))) > 0) {
		if (cache_copy &&
		    cache_used + (size_t)n <= (size_t)st.st_size) {
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

/**
 * @brief Render an HTML template and send it as HTTP response.
 * @param req Request context.
 * @param data Template descriptor for the page.
 * @param fallback_template Error string when rendering fails.
 * @return HTTP send status code.
 */
int
http_render_template(http_request_t *req, struct template_data *data,
		     const char *fallback_template)
{
	char *output = NULL;

	/* Try rendering with structured template data first. */
	if (template_render_with_data(data, &output) != 0) {
		/* If that fails and page content exists, try basic rendering.
		 */
		if (data->page_content &&
		    template_render(data->page_content, &output) != 0) {
			/* If all rendering paths fail, use fallback or return
			 * 500. */
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
