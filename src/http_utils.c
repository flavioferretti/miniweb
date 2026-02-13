/* http_utils.c - HTTP utility functions for libmicrohttpd */

#include "../include/http_utils.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Default error pages */
static const char *ERROR_404_HTML =
    "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
    "<title>404 Not Found</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"/static/custom.css\" />"
    "</head><body>"
    "<h1>404 - Page Not Found</h1>"
    "<p>The requested resource <code>%s</code> was not found on this "
    "server.</p>"
    "<hr><p><a href=\"/\">MiniWeb Server</a> on OpenBSD</p>"
    "</body></html>";

static const char *ERROR_500_HTML =
    "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
    "<title>500 Internal Server Error</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"/static/custom.css\" />"
    "</head><body>"
    "<h1>500 - Internal Server Error</h1>"
    "<p>Something went wrong processing your request.</p>"
    "<p><small>%s</small></p>"
    "<hr><p><a href=\"/\">MiniWeb Server</a> on OpenBSD</p>"
    "</body></html>";

static const char *ERROR_400_HTML =
    "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
    "<title>400 Bad Request</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"/static/custom.css\" />"
    "</head><body>"
    "<h1>400 - Bad Request</h1>"
    "<p>%s</p>"
    "<hr><p><a href=\"/\">MiniWeb Server</a> on OpenBSD</p>"
    "</body></html>";

static const char *ERROR_403_HTML =
    "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
    "<title>403 Forbidden</title>"
    "<link rel=\"stylesheet\" type=\"text/css\" href=\"/static/custom.css\" />"
    "</head><body>"
    "<h1>403 - Forbidden</h1>"
    "<p>You don't have permission to access this resource.</p>"
    "<hr><p><a href=\"/\">MiniWeb Server</a> on OpenBSD</p>"
    "</body></html>";

/* Create error response with HTML formatting */
struct MHD_Response *
http_error_response(unsigned int status_code, const char *format, ...)
{
	char buffer[2048];
	const char *template = NULL;
	va_list args;
	char *html;

	switch (status_code) {
	case MHD_HTTP_NOT_FOUND:
		template = ERROR_404_HTML;
		break;
	case MHD_HTTP_INTERNAL_SERVER_ERROR:
		template = ERROR_500_HTML;
		break;
	case MHD_HTTP_BAD_REQUEST:
		template = ERROR_400_HTML;
		break;
	case MHD_HTTP_FORBIDDEN:
		template = ERROR_403_HTML;
		break;
	default:
		snprintf(buffer, sizeof(buffer),
			 "<html><body><h1>%d Error</h1></body></html>",
			 status_code);
		html = strdup(buffer);
		if (!html) {
			html = strdup("<html><body><h1>Internal Server "
				      "Error</h1></body></html>");
		}
		goto create_response;
	}

	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), template, args);
	va_end(args);

	html = strdup(buffer);
	if (!html) {
		html = strdup(
		    "<html><body><h1>Internal Server Error</h1></body></html>");
	}

create_response: {
	struct MHD_Response *response = MHD_create_response_from_buffer(
	    strlen(html), html, MHD_RESPMEM_MUST_FREE);

	MHD_add_response_header(response, "Content-Type",
				"text/html; charset=utf-8");
	return response;
}
}

/* Simple text error response (for APIs) */
struct MHD_Response *
http_error_text(unsigned int status_code, const char *message)
{
	(void)status_code; /* Suppress unused parameter warning */
	const char *msg = message ? message : "Error";
	char *msg_copy = strdup(msg);

	struct MHD_Response *response = MHD_create_response_from_buffer(
	    strlen(msg_copy), msg_copy, MHD_RESPMEM_MUST_FREE);

	MHD_add_response_header(response, "Content-Type",
				"text/plain; charset=utf-8");
	return response;
}

/* JSON error response for APIs */
struct MHD_Response *
http_error_json(unsigned int status_code, const char *error_msg, int errno_val)
{
	(void)status_code; /* Suppress unused parameter warning */
	char json[512];

	if (errno_val != 0) {
		snprintf(json, sizeof(json), "{\"error\":\"%s\",\"errno\":%d}",
			 error_msg ? error_msg : "Unknown error", errno_val);
	} else {
		snprintf(json, sizeof(json), "{\"error\":\"%s\"}",
			 error_msg ? error_msg : "Unknown error");
	}

	char *json_copy = strdup(json);
	struct MHD_Response *response = MHD_create_response_from_buffer(
	    strlen(json_copy), json_copy, MHD_RESPMEM_MUST_FREE);

	MHD_add_response_header(response, "Content-Type",
				"application/json; charset=utf-8");
	MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");

	return response;
}

/* Queue error response */
int
http_queue_error(struct MHD_Connection *connection, unsigned int status_code,
		 const char *message)
{
	struct MHD_Response *response;

	const char *accept =
	    MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Accept");

	if (accept && strstr(accept, "application/json")) {
		response = http_error_json(status_code, message, 0);
	} else {
		response = http_error_response(status_code, "%s",
					       message ? message : "");
	}

	int ret = MHD_queue_response(connection, status_code, response);
	MHD_destroy_response(response);
	return ret;
}

/* Queue 400 Bad Request */
int
http_queue_400(struct MHD_Connection *connection, const char *message)
{
	return http_queue_error(connection, MHD_HTTP_BAD_REQUEST, message);
}

/* Queue 403 Forbidden */
int
http_queue_403(struct MHD_Connection *connection, const char *message)
{
	return http_queue_error(connection, MHD_HTTP_FORBIDDEN, message);
}

/* Queue 404 Not Found */
int
http_queue_404(struct MHD_Connection *connection, const char *path)
{
	struct MHD_Response *response = http_error_response(
	    MHD_HTTP_NOT_FOUND, "%s", path ? path : "unknown");
	int ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
	MHD_destroy_response(response);
	return ret;
}

/* Queue 500 Internal Server Error */
int
http_queue_500(struct MHD_Connection *connection, const char *details)
{
	struct MHD_Response *response =
	    http_error_response(MHD_HTTP_INTERNAL_SERVER_ERROR, "%s",
				details ? details : "Internal error");
	int ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
				     response);
	MHD_destroy_response(response);
	return ret;
}

/* JSON string escaping */
char *
json_escape_string(const char *src)
{
	if (!src)
		return strdup("");

	size_t len = strlen(src);
	char *dest = malloc(len * 2 + 1);
	if (!dest)
		return strdup("");

	size_t d = 0;
	for (size_t s = 0; src[s] != '\0' && d < len * 2; s++) {
		switch (src[s]) {
		case '"':
			dest[d++] = '\\';
			dest[d++] = '"';
			break;
		case '\\':
			dest[d++] = '\\';
			dest[d++] = '\\';
			break;
		case '\b':
			dest[d++] = '\\';
			dest[d++] = 'b';
			break;
		case '\f':
			dest[d++] = '\\';
			dest[d++] = 'f';
			break;
		case '\n':
			dest[d++] = '\\';
			dest[d++] = 'n';
			break;
		case '\r':
			dest[d++] = '\\';
			dest[d++] = 'r';
			break;
		case '\t':
			dest[d++] = '\\';
			dest[d++] = 't';
			break;
		default:
			dest[d++] = src[s];
			break;
		}
	}
	dest[d] = '\0';
	return dest;
}

/* Sanitize string for filesystem safety - MA NON PER I NOMI DEI COMANDI! */
void
sanitize_string(char *s)
{
	if (!s)
		return;
	while (*s) {
		/* Permetti + e altri caratteri validi nei nomi dei comandi */
		if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-' &&
		    *s != '_' && *s != '+') {
			*s = '_';
		}
		s++;
	}
}

/* Safe popen read with timeout and size limit */
char *
safe_popen_read_argv(const char *path, char *const argv[], size_t max_size,
			   int timeout_seconds)
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
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execv(path, argv);
		_exit(127);
	}

	close(pipefd[1]);

	size_t capacity = max_size > 0 ? max_size : 1;
	char *buffer = malloc(capacity + 1);
	if (!buffer) {
		close(pipefd[0]);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return NULL;
	}

	size_t total = 0;
	time_t deadline = time(NULL) + (timeout_seconds > 0 ? timeout_seconds : 5);
	int timed_out = 0;

	while (total < max_size) {
		time_t now = time(NULL);
		if (now >= deadline) {
			timed_out = 1;
			break;
		}
		int wait_ms = (int)((deadline - now) * 1000);
		struct pollfd pfd = {.fd = pipefd[0], .events = POLLIN};
		int pr = poll(&pfd, 1, wait_ms);
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
			if (n <= 0)
				break;
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

char *
safe_popen_read(const char *cmd, size_t max_size)
{
	char *const argv[] = {"sh", "-c", (char *)cmd, NULL};
	return safe_popen_read_argv("/bin/sh", argv, max_size, 5);
}
