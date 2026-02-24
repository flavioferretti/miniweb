#include <miniweb/net/worker.h>

#include <errno.h>
#include <strings.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <miniweb/http/handler.h>
#include <miniweb/router/routes.h>
#include <miniweb/router/urls.h>

#define MAX_KEEPALIVE_REQUESTS 64

/** Parse the HTTP request start line into method/path/version buffers. */
static int
parse_request_line(const char *buf, char *method, char *url, char *version)
{
	const char *sp1 = strchr(buf, ' ');
	if (!sp1 || sp1 == buf)
		return -1;
	const char *sp2 = strchr(sp1 + 1, ' ');
	const char *eol = sp2 ? strstr(sp2 + 1, "\r\n") : NULL;
	if (!sp2 || !eol || sp2 == sp1 + 1 || eol == sp2 + 1)
		return -1;
	size_t mlen = (size_t)(sp1 - buf), ulen = (size_t)(sp2 - (sp1 + 1));
	size_t vlen = (size_t)(eol - (sp2 + 1));
	if (mlen >= 32 || ulen >= 512 || vlen >= 32)
		return -1;
	memcpy(method, buf, mlen); method[mlen] = '\0';
	memcpy(url, sp1 + 1, ulen); url[ulen] = '\0';
	memcpy(version, sp2 + 1, vlen); version[vlen] = '\0';
	return 0;
}

/** Return pointer to CRLF-CRLF request header terminator when complete. */
static const char *
find_header_end(const char *buf)
{
	return strstr(buf, "\r\n\r\n");
}

/** Resolve keep-alive behavior from HTTP version and Connection header. */
static int
request_keep_alive(const char *buf, const char *version)
{
	int is_http11 = (strcmp(version, "HTTP/1.1") == 0);
	const char *p = buf;
	while ((p = strcasestr(p, "\r\nConnection:")) != NULL) {
		p += 13;
		while (*p == ' ' || *p == '\t') p++;
		if (strncasecmp(p, "close", 5) == 0) return 0;
		if (strncasecmp(p, "keep-alive", 10) == 0) return 1;
	}
	return is_http11 ? 1 : 0;
}

/** Emit a compact error response on fatal worker-side parsing/read errors. */
static void
send_error_response(int fd, int code, const char *msg)
{
	http_request_t req = {.fd = fd, .method = "GET", .url = "/",
	    .version = "HTTP/1.1", .keep_alive = 0};
	(void)http_send_error(&req, code, msg);
}

/** Close fd, remove kevent registration, and release pool bookkeeping. */
static void
close_connection(miniweb_worker_runtime_t *rt, int fd)
{
	struct kevent ev;
	EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	(void)kevent(*rt->kq_fd, &ev, 1, NULL, 0, NULL);
	close(fd);
	miniweb_connection_free(rt->pool, fd);
}

/** Re-enable EV_DISPATCH socket for next keep-alive request lifecycle. */
static int
try_rearm_keepalive(miniweb_worker_runtime_t *rt, miniweb_connection_t *conn)
{
	if (!conn || conn->requests_served >= MAX_KEEPALIVE_REQUESTS)
		return 0;
	conn->requests_served++;
	conn->bytes_read = 0;
	conn->buffer[0] = '\0';
	conn->last_activity = time(NULL);
	struct kevent ev;
	EV_SET(&ev, conn->fd, EVFILT_READ, EV_ENABLE, 0, 0, conn);
	return kevent(*rt->kq_fd, &ev, 1, NULL, 0, NULL) == 0;
}

/** Process queued sockets and execute HTTP handlers in worker threads. */
void *
miniweb_worker_thread(void *arg)
{
	miniweb_worker_runtime_t *rt = arg;
	while (*rt->running) {
		miniweb_connection_t *conn =
		    (miniweb_connection_t *)miniweb_work_queue_pop(rt->queue, rt->running);
		if (!conn)
			break;
		int fd = conn->fd, close_conn = 1, done = 0;
		while (!done) {
			ssize_t n = recv(fd, conn->buffer + conn->bytes_read,
			    (size_t)rt->config->max_req_size - conn->bytes_read - 1, 0);
			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					struct kevent ev;
					EV_SET(&ev, fd, EVFILT_READ, EV_ENABLE, 0, 0, conn);
					kevent(*rt->kq_fd, &ev, 1, NULL, 0, NULL);
					goto next;
				}
				break;
			}
			if (n == 0)
				break;
			conn->bytes_read += (size_t)n;
			conn->last_activity = time(NULL);
			conn->buffer[conn->bytes_read] = '\0';
			if (find_header_end(conn->buffer)) done = 1;
			else if (conn->bytes_read >= (size_t)rt->config->max_req_size - 1) {
				send_error_response(fd, 400, "Request Too Large");
				break;
			}
		}
		if (done) {
			char method[32] = {0}, path[512] = {0}, version[32] = {0};
			if (parse_request_line(conn->buffer, method, path, version) == 0) {
				int keep_alive = request_keep_alive(conn->buffer, version);
				http_handler_t handler = route_match(method, path);
				http_request_t req = {.fd = fd,.method = method,.url = path,
				    .version = version,.keep_alive = keep_alive,.buffer = conn->buffer,
				    .buffer_len = conn->bytes_read,.client_addr = &conn->addr};
				if (handler) handler(&req);
				else http_send_error(&req, route_path_known(path) ? 405 : 404,
				    route_path_known(path) ? "Method Not Allowed" : "Not Found");
				if (req.keep_alive && try_rearm_keepalive(rt, conn))
					close_conn = 0;
			} else send_error_response(fd, 400, "Bad Request");
		}
		if (close_conn)
			close_connection(rt, fd);
	next:;
	}
	return NULL;
}
