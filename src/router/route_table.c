
/* route_table.c - Route handlers with native kqueue interface */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <miniweb/http/handler.h>
#include <miniweb/http/utils.h>
#include <miniweb/core/config.h>
#include <miniweb/router/routes.h>
#include <miniweb/router/urls.h>

#define HOT_VIEW_CACHE_TTL_SEC 10
#define HOT_VIEW_CACHE_MAX 5

typedef struct {
	const char *path;
	char *body;
	time_t created_at;
} hot_view_cache_entry_t;

static hot_view_cache_entry_t g_hot_view_cache[HOT_VIEW_CACHE_MAX] = {
	{ .path = "/" },
	{ .path = "/docs" },
	{ .path = "/networking" },
	{ .path = "/packages" },
	{ .path = "/apiroot" },
};
static pthread_mutex_t g_hot_view_cache_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Return a cached hot-view entry for @p url, or NULL if absent/expired.
 * @param url Request URL to look up.
 * @return Pointer to the cache entry, or NULL.
 */
static hot_view_cache_entry_t *
find_hot_view_cache_entry(const char *path)
{
	for (size_t i = 0; i < HOT_VIEW_CACHE_MAX; i++) {
		if (strcmp(g_hot_view_cache[i].path, path) == 0)
			return &g_hot_view_cache[i];
	}
	return NULL;
}


/**
 * @brief View template handler.
 * @param req Request context for response generation.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
int
view_template_handler(http_request_t *req)
{
	time_t now = time(NULL);
	hot_view_cache_entry_t *cache_entry = NULL;

	/* --- Cache hit path --- */
	if (req->url && strchr(req->url, '?') == NULL) {
		pthread_mutex_lock(&g_hot_view_cache_lock);
		cache_entry = find_hot_view_cache_entry(req->url);
		if (cache_entry && cache_entry->body &&
			(now - cache_entry->created_at) <= HOT_VIEW_CACHE_TTL_SEC) {
			char *cached = strdup(cache_entry->body);
		pthread_mutex_unlock(&g_hot_view_cache_lock);
		if (!cached) {
			return http_send_error(req, 500, "Out of memory");
		}
		int ret = http_send_html(req, cached);
		free(cached);
		return ret;
			}
			pthread_mutex_unlock(&g_hot_view_cache_lock);
	}

	/* --- Cache miss: render --- */
	const struct view_route *view = find_view_route(req->method, req->url);
	if (view == NULL) {
		return http_send_error(req, 404, "Not Found");
	}

	struct template_data data = {
		.title           = view->title,
		.page_content    = view->page,
		.extra_head_file = view->extra_head,
		.extra_js_file   = view->extra_js,
	};

	char *output = NULL;
	if (template_render_with_data(&data, &output) != 0) {
		if (data.page_content &&
			template_render(data.page_content, &output) != 0) {
			return http_send_error(req, 500, "Template rendering failed");
			}
	}
	if (!output) {
		return http_send_error(req, 500, "Template rendering failed");
	}

	/* Cache owns its own strdup copy — independent from the response buffer. */
	if (cache_entry) {
		char *cache_copy = strdup(output);
		if (cache_copy) {
			pthread_mutex_lock(&g_hot_view_cache_lock);
			free(cache_entry->body);
			cache_entry->body = cache_copy;
			cache_entry->created_at = now;
			pthread_mutex_unlock(&g_hot_view_cache_lock);
		}
	}

	/* Response takes ownership of output and frees it (free_body = 1). */
	http_response_t *resp = http_response_create();
	if (!resp) {
		free(output);
		return -1;
	}
	http_response_set_body(resp, output, strlen(output), 1);
	int ret = http_response_send(req, resp);
	http_response_free(resp);
	return ret;
}

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

	 const char *mime = mime_type_for_path(path);

	 return http_send_file(req, fullpath, mime);
 }
