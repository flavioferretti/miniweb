/* routes.c - Route system implementation */

#include <errno.h>
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/routes.h"
#include "../include/template_engine.h"

/* Route table */
#define MAX_ROUTES 32
static struct route routes[MAX_ROUTES];
static size_t route_count = 0;

/* Route matching function */
route_handler_t
route_match(const char *method, const char *path)
{
	size_t i;

	/* First check exact route matches */
	for (i = 0; i < route_count; i++) {
		if (strcmp(routes[i].method, method) == 0 &&
		    strcmp(routes[i].path, path) == 0) {
			return routes[i].handler;
		}
	}

	/* Special case: static files ONLY if path starts with /static/ */
	if (strcmp(method, "GET") == 0 && strncmp(path, "/static/", 8) == 0) {
		return static_handler;
	}

	/* No handler found */
	return NULL;
}

/* Route registration */
static void
register_route(const char *method, const char *path, route_handler_t handler,
	       void *handler_cls)
{
	if (route_count >= MAX_ROUTES) {
		return;
	}

	routes[route_count].method = method;
	routes[route_count].path = path;
	routes[route_count].handler = handler;
	routes[route_count].handler_cls = handler_cls;
	route_count++;
}

/* Initialize all routes */
void
init_routes(void)
{
	/* Register built-in routes */
	register_route("GET", "/", index_handler, NULL);
	register_route("GET", "/info", info_handler, NULL);
	register_route("GET", "/favicon.ico", favicon_handler, NULL);
	register_route("GET", "/api/metrics", metrics_handler, NULL);
}

/* Index page handler */
int
index_handler(void *cls, struct MHD_Connection *connection, const char *url,
	      const char *method, const char *version, const char *upload_data,
	      size_t *upload_data_size, void **con_cls)
{
	(void)cls;
	(void)url;
	(void)method;
	(void)version;
	(void)upload_data;
	(void)upload_data_size;
	(void)con_cls;

	struct template_data data = {.title = "MiniWeb - Home",
				     .page_content = "index.html",
				     .extra_head_file = "info_extra_head.html",
				     .extra_js_file = "info_extra_js.html"};

	char *output = NULL;
	struct MHD_Response *response = NULL;
	int ret = MHD_NO;

	/* Render template */
	if (template_render_with_data(&data, &output) != 0) {
		/* Fallback to simple render */
		if (template_render("index.html", &output) != 0) {
			const char *error =
			    "Internal Server Error: Failed to render template";
			response = MHD_create_response_from_buffer(
			    strlen(error), (void *)error,
			    MHD_RESPMEM_PERSISTENT);
			MHD_add_response_header(response, "Content-Type",
						"text/html");
			ret = MHD_queue_response(connection,
						 MHD_HTTP_INTERNAL_SERVER_ERROR,
						 response);
			MHD_destroy_response(response);
			return ret;
		}
	}

	/* Create response */
	response = MHD_create_response_from_buffer(strlen(output), output,
						   MHD_RESPMEM_MUST_FREE);
	MHD_add_response_header(response, "Content-Type",
				"text/html; charset=utf-8");
	ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);

	return ret;
}

/* Info page handler */
int
info_handler(void *cls, struct MHD_Connection *connection, const char *url,
	     const char *method, const char *version, const char *upload_data,
	     size_t *upload_data_size, void **con_cls)
{
	(void)cls;
	(void)url;
	(void)method;
	(void)version;
	(void)upload_data;
	(void)upload_data_size;
	(void)con_cls;

	struct template_data data = {.title = "MiniWeb - Info",
				     .page_content = "info.html",
				     .extra_head_file = "info_extra_head.html",
				     .extra_js_file = "info_extra_js.html"};

	char *output = NULL;
	struct MHD_Response *response = NULL;
	int ret = MHD_NO;

	/* Render template */
	if (template_render_with_data(&data, &output) != 0) {
		/* Fallback to simple render */
		if (template_render("info.html", &output) != 0) {
			const char *error =
			    "Internal Server Error: Failed to render template";
			response = MHD_create_response_from_buffer(
			    strlen(error), (void *)error,
			    MHD_RESPMEM_PERSISTENT);
			MHD_add_response_header(response, "Content-Type",
						"text/html");
			ret = MHD_queue_response(connection,
						 MHD_HTTP_INTERNAL_SERVER_ERROR,
						 response);
			MHD_destroy_response(response);
			return ret;
		}
	}

	/* Create response */
	response = MHD_create_response_from_buffer(strlen(output), output,
						   MHD_RESPMEM_MUST_FREE);
	MHD_add_response_header(response, "Content-Type",
				"text/html; charset=utf-8");
	ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);

	return ret;
}

/* Favicon handler (from your original favicon_view) */
int
favicon_handler(void *cls, struct MHD_Connection *connection, const char *url,
		const char *method, const char *version,
		const char *upload_data, size_t *upload_data_size,
		void **con_cls)
{
	(void)cls;
	(void)url;
	(void)method;
	(void)version;
	(void)upload_data;
	(void)upload_data_size;
	(void)con_cls;

	FILE *f = NULL;
	char *buffer = NULL;
	long file_size;
	size_t bytes_read;
	const char *path = "static/favicon.svg";
	struct MHD_Response *response = NULL;
	int ret = MHD_NO;

	f = fopen(path, "rb");
	if (!f) {
		const char *not_found = "Favicon not found";
		response = MHD_create_response_from_buffer(
		    strlen(not_found), (void *)not_found,
		    MHD_RESPMEM_PERSISTENT);
		MHD_add_response_header(response, "Content-Type", "text/plain");
		ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND,
					 response);
		MHD_destroy_response(response);
		return ret;
	}

	/* Determine file size */
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		const char *error = "Error reading favicon";
		response = MHD_create_response_from_buffer(
		    strlen(error), (void *)error, MHD_RESPMEM_PERSISTENT);
		MHD_add_response_header(response, "Content-Type", "text/plain");
		ret = MHD_queue_response(
		    connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
		MHD_destroy_response(response);
		return ret;
	}

	file_size = ftell(f);
	if (file_size < 0) {
		fclose(f);
		const char *error = "Error reading favicon";
		response = MHD_create_response_from_buffer(
		    strlen(error), (void *)error, MHD_RESPMEM_PERSISTENT);
		MHD_add_response_header(response, "Content-Type", "text/plain");
		ret = MHD_queue_response(
		    connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
		MHD_destroy_response(response);
		return ret;
	}

	rewind(f);

	/* Allocate memory for the file content */
	buffer = malloc(file_size + 1);
	if (!buffer) {
		fclose(f);
		const char *error = "Out of memory";
		response = MHD_create_response_from_buffer(
		    strlen(error), (void *)error, MHD_RESPMEM_PERSISTENT);
		MHD_add_response_header(response, "Content-Type", "text/plain");
		ret = MHD_queue_response(
		    connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
		MHD_destroy_response(response);
		return ret;
	}

	/* Read the entire file into the buffer */
	bytes_read = fread(buffer, 1, file_size, f);
	if (bytes_read != (size_t)file_size) {
		free(buffer);
		fclose(f);
		const char *error = "Error reading favicon";
		response = MHD_create_response_from_buffer(
		    strlen(error), (void *)error, MHD_RESPMEM_PERSISTENT);
		MHD_add_response_header(response, "Content-Type", "text/plain");
		ret = MHD_queue_response(
		    connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
		MHD_destroy_response(response);
		return ret;
	}

	buffer[bytes_read] = '\0';
	fclose(f);

	/* Prepare response and set proper SVG MIME type */
	response = MHD_create_response_from_buffer(bytes_read, buffer,
						   MHD_RESPMEM_MUST_FREE);
	MHD_add_response_header(response, "Content-Type", "image/svg+xml");
	ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);

	return ret;
}

/* Static file handler - ONLY serves from /static/ URLs */
/* Static file handler - ONLY serves from /static/ URLs */
int
static_handler(void *cls, struct MHD_Connection *connection, const char *url,
	       const char *method, const char *version, const char *upload_data,
	       size_t *upload_data_size, void **con_cls)
{
	(void)cls;
	(void)method;
	(void)version;
	(void)upload_data;
	(void)upload_data_size;
	(void)con_cls;

	char path[512];
	struct MHD_Response *response = NULL;
	int ret = MHD_NO;
	FILE *f = NULL;
	char *buffer = NULL;

	/* Security check: prevent directory traversal */
	if (strstr(url, "..") != NULL || strstr(url, "//") != NULL) {
		const char *forbidden =
		    "<html><body><h1>403 Forbidden</h1></body></html>";
		response = MHD_create_response_from_buffer(
		    strlen(forbidden), (void *)forbidden,
		    MHD_RESPMEM_PERSISTENT);
		MHD_add_response_header(response, "Content-Type", "text/html");
		ret = MHD_queue_response(connection, MHD_HTTP_FORBIDDEN,
					 response);
		MHD_destroy_response(response);
		return ret;
	}

	/* Serve ONLY from /static/ directory */
	if (strncmp(url, "/static/", 8) != 0) {
		/* Not a static file request - return 404 */
		const char *not_found =
		    "<html><body><h1>404 Not Found</h1></body></html>";
		response = MHD_create_response_from_buffer(
		    strlen(not_found), (void *)not_found,
		    MHD_RESPMEM_PERSISTENT);
		MHD_add_response_header(response, "Content-Type", "text/html");
		ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND,
					 response);
		MHD_destroy_response(response);
		return ret;
	}

	/* Convert URL to filesystem path: /static/test.txt -> static/test.txt
	 */
	snprintf(path, sizeof(path), "static%s",
		 url + 7); /* +7 per saltare "/static" */

	f = fopen(path, "rb");
	if (!f) {
		/* File not found */
		const char *not_found =
		    "<html><body><h1>404 Not Found</h1></body></html>";
		response = MHD_create_response_from_buffer(
		    strlen(not_found), (void *)not_found,
		    MHD_RESPMEM_PERSISTENT);
		MHD_add_response_header(response, "Content-Type", "text/html");
		ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND,
					 response);
		MHD_destroy_response(response);
		return ret;
	}

	/* Get file size */
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		const char *error = "Internal Server Error: Cannot seek file";
		response = MHD_create_response_from_buffer(
		    strlen(error), (void *)error, MHD_RESPMEM_PERSISTENT);
		MHD_add_response_header(response, "Content-Type", "text/plain");
		ret = MHD_queue_response(
		    connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
		MHD_destroy_response(response);
		return ret;
	}

	long size = ftell(f);
	if (size < 0) {
		fclose(f);
		const char *error =
		    "Internal Server Error: Cannot get file size";
		response = MHD_create_response_from_buffer(
		    strlen(error), (void *)error, MHD_RESPMEM_PERSISTENT);
		MHD_add_response_header(response, "Content-Type", "text/plain");
		ret = MHD_queue_response(
		    connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
		MHD_destroy_response(response);
		return ret;
	}

	rewind(f);

	/* Allocate buffer for file content */
	buffer = malloc(size + 1);
	if (!buffer) {
		fclose(f);
		const char *error = "Internal Server Error: Out of memory";
		response = MHD_create_response_from_buffer(
		    strlen(error), (void *)error, MHD_RESPMEM_PERSISTENT);
		MHD_add_response_header(response, "Content-Type", "text/plain");
		ret = MHD_queue_response(
		    connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
		MHD_destroy_response(response);
		return ret;
	}

	/* Read file content */
	size_t bytes_read = fread(buffer, 1, size, f);
	if (bytes_read != (size_t)size) {
		free(buffer);
		fclose(f);
		const char *error = "Internal Server Error: Cannot read file";
		response = MHD_create_response_from_buffer(
		    strlen(error), (void *)error, MHD_RESPMEM_PERSISTENT);
		MHD_add_response_header(response, "Content-Type", "text/plain");
		ret = MHD_queue_response(
		    connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
		MHD_destroy_response(response);
		return ret;
	}

	buffer[bytes_read] = '\0'; /* Null-terminate for text files */
	fclose(f);

	/* Create response with file content */
	response = MHD_create_response_from_buffer(bytes_read, buffer,
						   MHD_RESPMEM_MUST_FREE);

	/* Set content type based on file extension */
	const char *ext = strrchr(url, '.');
	if (ext) {
		if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) {
			MHD_add_response_header(response, "Content-Type",
						"text/html; charset=utf-8");
		} else if (strcmp(ext, ".css") == 0) {
			MHD_add_response_header(response, "Content-Type",
						"text/css; charset=utf-8");
		} else if (strcmp(ext, ".js") == 0) {
			MHD_add_response_header(
			    response, "Content-Type",
			    "application/javascript; charset=utf-8");
		} else if (strcmp(ext, ".svg") == 0) {
			MHD_add_response_header(response, "Content-Type",
						"image/svg+xml");
		} else if (strcmp(ext, ".png") == 0) {
			MHD_add_response_header(response, "Content-Type",
						"image/png");
		} else if (strcmp(ext, ".jpg") == 0 ||
			   strcmp(ext, ".jpeg") == 0) {
			MHD_add_response_header(response, "Content-Type",
						"image/jpeg");
		} else if (strcmp(ext, ".ico") == 0) {
			MHD_add_response_header(response, "Content-Type",
						"image/x-icon");
		} else if (strcmp(ext, ".txt") == 0 ||
			   strcmp(ext, ".md") == 0) {
			MHD_add_response_header(response, "Content-Type",
						"text/plain; charset=utf-8");
		} else if (strcmp(ext, ".json") == 0) {
			MHD_add_response_header(
			    response, "Content-Type",
			    "application/json; charset=utf-8");
		} else if (strcmp(ext, ".pdf") == 0) {
			MHD_add_response_header(response, "Content-Type",
						"application/pdf");
		} else if (strcmp(ext, ".xml") == 0) {
			MHD_add_response_header(
			    response, "Content-Type",
			    "application/xml; charset=utf-8");
		} else if (strcmp(ext, ".zip") == 0) {
			MHD_add_response_header(response, "Content-Type",
						"application/zip");
		} else if (strcmp(ext, ".gz") == 0) {
			MHD_add_response_header(response, "Content-Type",
						"application/gzip");
		}
	} else {
		/* Default content type for files without extension */
		MHD_add_response_header(response, "Content-Type",
					"application/octet-stream");
	}

	/* Add cache headers for static files (optional) */
	MHD_add_response_header(response, "Cache-Control",
				"public, max-age=3600");

	/* Send response */
	ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);

	return ret;
}
