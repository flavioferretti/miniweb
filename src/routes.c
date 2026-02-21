/* routes.c - Route handlers with native kqueue interface */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../include/http_handler.h"
#include "../include/config.h"
#include "../include/man.h"
#include "../include/metrics.h"
#include "../include/networking.h"
#include "../include/routes.h"
#include "../include/urls.h"

/* Generic template-backed view handler */
/**
 * @brief View template handler.
 * @param req Request context for response generation.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
int
view_template_handler(http_request_t *req)
{
	const struct view_route *view = find_view_route(req->method, req->url);

	if (view == NULL)
		return http_send_error(req, 404, "Not Found");

	struct template_data data = {
		.title = view->title,
		.page_content = view->page,
		.extra_head_file = view->extra_head,
		.extra_js_file = view->extra_js,
	};

	return http_render_template(req, &data, "Template rendering failed");
}

/* Favicon handler */
/**
 * @brief Favicon handler.
 * @param req Request context for response generation.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
int
favicon_handler(http_request_t *req)
{
	char favicon_path[512];
	snprintf(favicon_path, sizeof(favicon_path), "%s/assets/favicon.svg", config_static_dir);
	return http_send_file(req, favicon_path, "image/svg+xml");
}

/* Static file handler */
/* static_handler - Serve static CSS, JS, image, and HTML assets. */
/* routes.c - static file serving helpers. */
/**
 * @brief Static handler.
 * @param req Request context for response generation.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
int
static_handler(http_request_t *req)
{
	// req->url is like "/static/css/style.css"
	// Remove the "/static/" prefix and serve from the configured static directory.
	const char *path = req->url;

	// Skip the "/static/" prefix.
	if (strncmp(path, "/static/", 8) == 0) {
		path += 8;  // now path is "css/style.css"
	}

	// Prevent directory traversal.
	if (strstr(path, "..") || strstr(path, "//")) {
		return http_send_error(req, 403, "Forbidden");
	}

	// Build absolute file path.
	char fullpath[512];
	snprintf(fullpath, sizeof(fullpath), "%s/%s", config_static_dir, path);

	// printf("DEBUG: static_handler trying to serve: %s\n", fullpath);

	// Determine MIME type.
	const char *mime = "application/octet-stream";
	const char *ext = strrchr(path, '.');
	if (ext) {
		if (strcmp(ext, ".html") == 0) mime = "text/html";
		else if (strcmp(ext, ".css") == 0) mime = "text/css";
		else if (strcmp(ext, ".js") == 0) mime = "application/javascript";
		else if (strcmp(ext, ".png") == 0) mime = "image/png";
		else if (strcmp(ext, ".svg") == 0) mime = "image/svg+xml";
		else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) mime = "image/jpeg";
		else if (strcmp(ext, ".gif") == 0) mime = "image/gif";
		else if (strcmp(ext, ".ico") == 0) mime = "image/x-icon";
		else if (strcmp(ext, ".pdf") == 0) mime = "application/pdf";
		else if (strcmp(ext, ".ps") == 0) mime = "application/postscript";
		else if (strcmp(ext, ".md") == 0) mime = "text/markdown; charset=utf-8";
		else if (strcmp(ext, ".txt") == 0) mime = "text/plain; charset=utf-8";
	}

	return http_send_file(req, fullpath, mime);
}
