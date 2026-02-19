/* routes.c - Route handlers with native kqueue interface */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../include/http_handler.h"
#include "../include/man.h"
#include "../include/metrics.h"
#include "../include/networking.h"
#include "../include/routes.h"
#include "../include/template_engine.h"
#include "../include/urls.h"

/* Template rendering helper */
int
render_template_response(http_request_t *req, struct template_data *data)
{
	char *output = NULL;

	if (template_render_with_data(data, &output) != 0) {
		if (data->page_content &&
			template_render(data->page_content, &output) != 0) {
			return http_send_error(req, 500, "Template rendering failed");
			}
	}

	http_response_t *resp = http_response_create();
	http_response_set_body(resp, output, strlen(output), 1); /* Free output */

	int ret = http_response_send(req, resp);
	http_response_free(resp);

	return ret;
}

/* Generic template-backed view handler */
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

	return render_template_response(req, &data);
}

/* Favicon handler */
int
favicon_handler(http_request_t *req)
{
	return http_send_file(req, "static/assets/favicon.svg", "image/svg+xml");
}

/* Static file handler */
/* static_handler - Gestisce CSS, JS, Immagini e HTML statici */
/* routes.c - Gestione file statici */
int
static_handler(http_request_t *req)
{
	// req->url è tipo "/static/css/style.css"
	// Dobbiamo rimuovere "/static/" e servire dalla directory giusta
	const char *path = req->url;

	// Salta il prefisso "/static/"
	if (strncmp(path, "/static/", 8) == 0) {
		path += 8;  // ora path è "css/style.css"
	}

	// Prevenzione Directory Traversal
	if (strstr(path, "..") || strstr(path, "//")) {
		return http_send_error(req, 403, "Forbidden");
	}

	// Costruisci il percorso completo
	char fullpath[512];
	snprintf(fullpath, sizeof(fullpath), "static/%s", path);

	// printf("DEBUG: static_handler trying to serve: %s\n", fullpath);

	// Determina il MIME type
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
	}

	return http_send_file(req, fullpath, mime);
}
