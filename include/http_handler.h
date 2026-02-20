/* http_handler.h - Native kqueue HTTP handler interface */

#ifndef HTTP_HANDLER_H
#define HTTP_HANDLER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <stddef.h>
#include "template_engine.h"

/* HTTP request structure.
 *
 * hdr_scratch / ip_scratch are per-request buffers used by
 * http_request_get_header() and http_request_get_client_ip() so that
 * those helpers are thread-safe without any static storage.
 */
typedef struct http_request {
	int fd;                          /* Client socket */
	const char *method;              /* GET, POST, etc. */
	const char *url;                 /* Request path */
	const char *version;             /* HTTP/1.1 */
	int keep_alive;                  /* 1 if connection should stay open */
	const char *buffer;              /* Full raw request buffer */
	size_t buffer_len;               /* Bytes in buffer */
	struct sockaddr_in *client_addr; /* Peer address */

	/* Per-request scratch space — written by helper functions,
	 * valid only for the lifetime of the request.            */
	char hdr_scratch[1024];
	char ip_scratch[INET_ADDRSTRLEN];
} http_request_t;

/* HTTP response structure */
typedef struct http_response {
	int status_code;
	const char *content_type;
	char *body;
	size_t body_len;
	int free_body;
	char headers[2048];
	size_t headers_len;
} http_response_t;

/* Handler function type */
typedef int (*http_handler_t)(http_request_t *req);

/** Allocate and initialize a new HTTP response object. */
http_response_t *http_response_create(void);

/** Set response status code. */
void http_response_set_status(http_response_t *resp, int code);

/** Set response body pointer, byte length, and ownership flag. */
void http_response_set_body(http_response_t *resp, char *body,
								size_t len, int must_free);

/** Append one HTTP header line to the response. */
void http_response_add_header(http_response_t *resp,
								  const char *name, const char *value);

/** Serialize and send a complete HTTP response to the client socket. */
int  http_response_send(http_request_t *req, http_response_t *resp);

/** Free response object and owned body buffer when configured. */
void http_response_free(http_response_t *resp);

/** Lookup one request header value by case-insensitive name. */
const char *http_request_get_header(http_request_t *req, const char *name);

/** Return client IP address, honoring trusted proxy forwarding headers. */
const char *http_request_get_client_ip(http_request_t *req);

/** Return 1 when request is HTTPS (direct or forwarded), else 0. */
int         http_request_is_https(http_request_t *req);

/** Send a plain-text error response with the given status code. */
int http_send_error(http_request_t *req, int status_code, const char *message);

/** Send JSON with HTTP 200 status. */
int http_send_json (http_request_t *req, const char *json);

/** Send HTML with HTTP 200 status. */
int http_send_html (http_request_t *req, const char *html);

/** Send a file from disk with explicit content type. */
int http_send_file (http_request_t *req, const char *path,
						const char *content_type);

/** Render a template response with optional fallback template name. */
int http_render_template(http_request_t *req, struct template_data *data,
							 const char *fallback_template);

#endif /* HTTP_HANDLER_H */
