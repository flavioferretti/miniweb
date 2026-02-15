/* routes.c - Route system implementation */

#include <errno.h>
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/http_utils.h"
#include "../include/man.h"
#include "../include/metrics.h"
#include "../include/routes.h"
#include "../include/urls.h"
#include "../include/template_engine.h"

/* Route matching function */
route_handler_t
route_match(const char *method, const char *path)
{
	return find_route_match(method, path);
}


/* Template rendering helper */
static int
render_template_response(struct MHD_Connection *connection,
			 struct template_data *data,
			 const char *fallback_template)
{
	char *output = NULL;
	int ret;

	if (template_render_with_data(data, &output) != 0) {
		if (fallback_template &&
		    template_render(fallback_template, &output) != 0) {
			return http_queue_500(connection,
					      "Template rendering failed");
		}
	}

	struct MHD_Response *response = MHD_create_response_from_buffer(
	    strlen(output), output, MHD_RESPMEM_MUST_FREE);
	MHD_add_response_header(response, "Content-Type",
				"text/html; charset=utf-8");
	ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);

	return ret;
}

/* Dashboard page handler */
int
dashboard_handler(void *cls, struct MHD_Connection *connection, const char *url,
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

	struct template_data data = {
	    .title = "MiniWeb - Dashboard",
	    .page_content = "dashboard.html",
	    .extra_head_file = "dashboard_extra_head.html",
	    .extra_js_file = "dashboard_extra_js.html"};

	return render_template_response(connection, &data, "dashboard.html");
}

/* API root page handler */
int
apiroot_handler(void *cls, struct MHD_Connection *connection, const char *url,
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

	struct template_data data = {.title = "MiniWeb - API Root",
		.page_content = "api.html",
		.extra_head_file = "api_extra_head.html",
		.extra_js_file = "api_extra_js.html"};

		return render_template_response(connection, &data, "api.html");
}

/* Documentation page handler */
int
man_handler(void *cls, struct MHD_Connection *connection, const char *url,
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

	struct template_data data = {.title = "MiniWeb - Documentation",
				     .page_content = "docs.html",
				     .extra_head_file = "docs_extra_head.html",
				     .extra_js_file = "docs_extra_js.html"};

	return render_template_response(connection, &data, "docs.html");
}

/* Favicon handler */
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

	const char *path = "static/assets/favicon.svg";
	FILE *f = fopen(path, "rb");

	if (!f) {
		return http_queue_404(connection, path);
	}

	/* Get file size */
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return http_queue_500(connection, "Cannot seek favicon");
	}

	long file_size = ftell(f);
	if (file_size < 0) {
		fclose(f);
		return http_queue_500(connection, "Cannot get favicon size");
	}

	rewind(f);

	char *buffer = malloc(file_size + 1);
	if (!buffer) {
		fclose(f);
		return http_queue_500(connection, "Out of memory");
	}

	size_t bytes_read = fread(buffer, 1, file_size, f);
	if (bytes_read != (size_t)file_size) {
		free(buffer);
		fclose(f);
		return http_queue_500(connection, "Cannot read favicon");
	}

	buffer[bytes_read] = '\0';
	fclose(f);

	struct MHD_Response *response = MHD_create_response_from_buffer(
	    bytes_read, buffer, MHD_RESPMEM_MUST_FREE);
	MHD_add_response_header(response, "Content-Type", "image/svg+xml");

	int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);
	return ret;
}

/* Static file handler - ONLY serves from /static/ */
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

	/* Security check: prevent directory traversal */
	if (strstr(url, "..") != NULL || strstr(url, "//") != NULL) {
		return http_queue_403(connection,
				      "Directory traversal detected");
	}

	/* Serve ONLY from /static/ directory */
	if (strncmp(url, "/static/", 8) != 0) {
		return http_queue_404(connection, url);
	}

	char path[512];
	snprintf(path, sizeof(path), "static%s", url + 7);

	FILE *f = fopen(path, "rb");
	if (!f) {
		return http_queue_404(connection, url);
	}

	/* Get file size */
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return http_queue_500(connection, "Cannot seek file");
	}

	long size = ftell(f);
	if (size < 0) {
		fclose(f);
		return http_queue_500(connection, "Cannot get file size");
	}

	rewind(f);

	char *buffer = malloc(size + 1);
	if (!buffer) {
		fclose(f);
		return http_queue_500(connection, "Out of memory");
	}

	size_t bytes_read = fread(buffer, 1, size, f);
	if (bytes_read != (size_t)size) {
		free(buffer);
		fclose(f);
		return http_queue_500(connection, "Cannot read file");
	}

	buffer[bytes_read] = '\0';
	fclose(f);

	struct MHD_Response *response = MHD_create_response_from_buffer(
	    bytes_read, buffer, MHD_RESPMEM_MUST_FREE);

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
		} else {
			MHD_add_response_header(response, "Content-Type",
						"application/octet-stream");
		}
	}

	MHD_add_response_header(response, "Cache-Control",
				"public, max-age=3600");

	int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);
	return ret;
}
