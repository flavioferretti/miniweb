/* http_handler.c - HTTP handler utilities */

#include "http_handler.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>

/* Create response */
http_response_t *
http_response_create(void)
{
	http_response_t *resp = calloc(1, sizeof(http_response_t));
	if (resp) {
		resp->status_code = 200;
		resp->content_type = "text/html; charset=utf-8";
	}
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
				/* Socket buffer pieno, aspettiamo un po' */
				if (retries++ > 100) { /* Evita loop infinito */
					fprintf(stderr, "[write_all] Too many "
							"EAGAIN retries\n");
					return -1;
				}
				struct pollfd pfd = {.fd = fd,
						     .events = POLLOUT};
				poll(&pfd, 1, 10); /* Aspetta 10ms */
				continue;
			}
			fprintf(stderr, "[write_all] Error: %s\n",
				strerror(errno));
			return -1;
		}
		if (w == 0) {
			fprintf(stderr, "[write_all] Connection closed\n");
			return -1;
		}
		p += w;
		remaining -= (size_t)w;
		retries = 0; /* Reset retries dopo un write riuscito */
	}

	if (n > 0) { /* Log solo se abbiamo scritto qualcosa */
		fprintf(stderr, "[write_all] Wrote %zu bytes\n", n);
	}
	return 0;
}

/* Send HTTP response */
/* Aggiungi questo debug temporaneo in http_response_send */
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
						  "Connection: close\r\n"
						  "Server: MiniWeb/kqueue\r\n",
					   resp->status_code, status_text,
					   resp->content_type,
					   resp->body_len);

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

	fprintf(stderr, "[HTTP] Sending response: status=%d, type=%s, length=%zu\n",
			resp->status_code, resp->content_type, resp->body_len);

	/* Invia header */
	if (write_all(req->fd, header, header_len) < 0) {
		fprintf(stderr, "[HTTP] Error writing headers\n");
		return -1;
	}

	/* Per file grandi, diamo al client il tempo di processare gli header */
	if (resp->body_len > 65536) {  /* > 64KB */
		usleep(5000);  /* 5ms delay */
	}

	/* Invia body */
	if (resp->body && resp->body_len > 0) {
		if (write_all(req->fd, resp->body, resp->body_len) < 0) {
			fprintf(stderr, "[HTTP] Error writing body\n");
			return -1;
		}
	}

	return 0;
}
// int
// http_response_send(http_request_t *req, http_response_t *resp)
// {
// 	char header[4096];
// 	int header_len;
//
// 	const char *status_text;
// 	switch (resp->status_code) {
// 		case 200: status_text = "OK"; break;
// 		case 400: status_text = "Bad Request"; break;
// 		case 403: status_text = "Forbidden"; break;
// 		case 404: status_text = "Not Found"; break;
// 		case 500: status_text = "Internal Server Error"; break;
// 		case 503: status_text = "Service Unavailable"; break;
// 		default:  status_text = "Unknown"; break;
// 	}
//
// 	/* Build HTTP header */
// 	header_len = snprintf(header, sizeof(header),
// 						  "HTTP/1.1 %d %s\r\n"
// 						  "Content-Type: %s\r\n"
// 						  "Content-Length: %zu\r\n"
// 						  "Connection: close\r\n"
// 						  "Server: MiniWeb/kqueue\r\n",
// 					   resp->status_code, status_text,
// 					   resp->content_type,
// 					   resp->body_len);
//
// 	/* Append custom headers if they fit */
// 	if (resp->headers_len > 0) {
// 		int space = (int)sizeof(header) - header_len;
// 		if ((int)resp->headers_len < space) {
// 			memcpy(header + header_len, resp->headers,
// resp->headers_len); 			header_len += (int)resp->headers_len;
// 		}
// 	}
//
// 	/* Terminate header section */
// 	header_len += snprintf(header + header_len,
// 						   sizeof(header) - header_len,
// "\r\n");
//
// 	if (write_all(req->fd, header, header_len) < 0)
// 		return -1;
//
// 	if (resp->body && resp->body_len > 0)
// 		if (write_all(req->fd, resp->body, resp->body_len) < 0)
// 			return -1;
//
// 	return 0;
// }

/* Free response */
void
http_response_free(http_response_t *resp)
{
	if (!resp)
		return;

	/* Se il flag free_body è 1, dobbiamo liberare il buffer allocato (es.
	 * da fread) */
	if (resp->free_body && resp->body) {
		free(resp->body);
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
	    "<h1>%d Error</h1>"
	    "<p>%s</p>"
	    "<hr><p><a href=\"/\">MiniWeb</a> on OpenBSD</p>"
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
	size_t json_len = strlen(json);
	char header[512];
	int header_len = snprintf(header, sizeof(header),
				  "HTTP/1.1 200 OK\r\n"
				  "Content-Type: application/json\r\n"
				  "Content-Length: %zu\r\n"
				  "Connection: close\r\n"
				  "\r\n",
				  json_len);

	if (write_all(req->fd, header, header_len) < 0)
		return -1;
	if (write_all(req->fd, json, json_len) < 0)
		return -1;
	return 0;
}

/* Send HTML response */
int
http_send_html(http_request_t *req, const char *html)
{
	size_t html_len = strlen(html);
	char header[512];
	int header_len = snprintf(header, sizeof(header),
				  "HTTP/1.1 200 OK\r\n"
				  "Content-Type: text/html; charset=utf-8\r\n"
				  "Content-Length: %zu\r\n"
				  "Connection: close\r\n"
				  "\r\n",
				  html_len);

	if (write_all(req->fd, header, header_len) < 0)
		return -1;
	if (write_all(req->fd, html, html_len) < 0)
		return -1;
	return 0;
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

	char header[4096];
	int header_len = snprintf(header, sizeof(header),
				  "HTTP/1.1 200 OK\r\n"
				  "Content-Type: %s\r\n"
				  "Content-Length: %lld\r\n"
				  "Connection: close\r\n"
				  "\r\n",
				  mime, (long long)st.st_size);

	if (write_all(req->fd, header, header_len) < 0) {
		close(fd);
		return -1;
	}

	char file_buf[8192];
	ssize_t n;
	while ((n = read(fd, file_buf, sizeof(file_buf))) > 0) {
		ssize_t total = 0;
		while (total < n) {
			ssize_t w = write(req->fd, file_buf + total, n - total);
			if (w < 0) {
				close(fd);
				return -1;
			}
			total += w;
		}
	}

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

	/* Tenta il rendering con i dati */
	if (template_render_with_data(data, &output) != 0) {
		/* Se fallisce e abbiamo un contenuto di pagina, prova il
		 * rendering semplice */
		if (data->page_content &&
		    template_render(data->page_content, &output) != 0) {
			/* Se fallisce tutto, usa il fallback o manda 500 */
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

	/* Il parametro '1' indica a http_response_set_body di liberare la
	 * memoria di 'output' */
	http_response_set_body(resp, output, strlen(output), 1);

	int ret = http_response_send(req, resp);
	http_response_free(resp);

	return ret;
}
