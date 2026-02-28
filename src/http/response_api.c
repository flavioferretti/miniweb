#include <miniweb/http/response_internal.h>

#include <miniweb/core/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

http_response_t *
http_response_create(void)
{
	http_response_t *resp;

	http_handler_globals_init_once();
	resp = http_response_pool_acquire();
	if (!resp) {
		resp = calloc(1, sizeof(*resp));
		if (!resp)
			return NULL;
		resp->pool_shard_idx = -1;
	}

	resp->status_code = 200;
	resp->content_type = "text/html; charset=utf-8";
	return resp;
}

void
http_response_set_status(http_response_t *resp, int code)
{
	resp->status_code = code;
}

void
http_response_set_body(http_response_t *resp, char *body, size_t len,
    int must_free)
{
	resp->body = body;
	resp->body_len = len;
	resp->free_body = must_free;
}

void
http_response_add_header(http_response_t *resp, const char *name,
    const char *value)
{
	int len;
	size_t space;

	if (resp->headers_len >= sizeof(resp->headers))
		return;
	space = sizeof(resp->headers) - resp->headers_len;
	len = snprintf(resp->headers + resp->headers_len, space,
	    "%s: %s\r\n", name, value);
	if (len > 0 && (size_t)len < space)
		resp->headers_len += (size_t)len;
	else if (space > 0)
		resp->headers_len = sizeof(resp->headers) - 1;
}

int
http_response_send(http_request_t *req, http_response_t *resp)
{
	char header[4096];
	struct iovec iov[2];
	int header_len;
	size_t space;
	size_t used;

	header_len = snprintf(header, sizeof(header),
	    "HTTP/1.1 %d %s\r\n"
	    "Content-Type: %s\r\n"
	    "Content-Length: %zu\r\n"
	    "Connection: %s\r\n"
	    "Server: MiniWeb/kqueue\r\n",
	    resp->status_code, http_response_status_text(resp->status_code),
	    resp->content_type, resp->body_len,
	    req->keep_alive ? "keep-alive" : "close");
	if (header_len < 0 || (size_t)header_len >= sizeof(header))
		return -1;

	if (resp->headers_len > 0) {
		used = (size_t)header_len;
		space = sizeof(header) - used;
		if (resp->headers_len < space) {
			memcpy(header + used, resp->headers, resp->headers_len);
			header_len += (int)resp->headers_len;
		}
	}
	if ((size_t)header_len + 2 >= sizeof(header))
		return -1;
	header[header_len++] = '\r';
	header[header_len++] = '\n';

	iov[0].iov_base = header;
	iov[0].iov_len = (size_t)header_len;
	iov[1].iov_base = resp->body;
	iov[1].iov_len = resp->body_len;

	if (resp->body && resp->body_len > 0) {
		if (http_response_writev_all(req->fd, iov, 2) < 0) {
			log_error("[HTTP] Error writing response");
			return -1;
		}
	} else if (http_response_write_all(req->fd, header, (size_t)header_len) <
	    0) {
		log_error("[HTTP] Error writing headers");
		return -1;
	}

	return 0;
}

void
http_response_free(http_response_t *resp)
{
	if (!resp)
		return;
	http_handler_globals_init_once();

	if (resp->free_body && resp->body)
		free(resp->body);

	if (http_response_pool_release(resp))
		return;
	free(resp);
}
