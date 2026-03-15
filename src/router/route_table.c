
/* route_table.c - Route handlers with native kqueue interface */

#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

/** @brief html_escape function. */
static char *
html_escape(const char *s)
{
	const char *p;
	char *out;
	char *w;
	size_t len = 0;

	if (!s)
		return strdup("");

	for (p = s; *p; p++) {
		switch (*p) {
		case '&': len += 5; break;
		case '<':
		case '>': len += 4; break;
		case '"': len += 6; break;
		default: len++; break;
		}
	}

	out = malloc(len + 1);
	if (!out)
		return NULL;

	w = out;
	for (p = s; *p; p++) {
		switch (*p) {
		case '&': memcpy(w, "&amp;", 5); w += 5; break;
		case '<': memcpy(w, "&lt;", 4); w += 4; break;
		case '>': memcpy(w, "&gt;", 4); w += 4; break;
		case '"': memcpy(w, "&quot;", 6); w += 6; break;
		default: *w++ = *p; break;
		}
	}
	*w = '\0';
	return out;
}

/** @brief append_str function. */
static int
append_str(char **buf, size_t *cap, size_t *len, const char *s)
{
	size_t slen;
	char *tmp;

	if (!buf || !*buf || !cap || !len || !s)
		return -1;
	slen = strlen(s);
	if (*len + slen + 1 > *cap) {
		size_t ncap = *cap;
		while (*len + slen + 1 > ncap)
			ncap *= 2;
		tmp = realloc(*buf, ncap);
		if (!tmp)
			return -1;
		*buf = tmp;
		*cap = ncap;
	}
	memcpy(*buf + *len, s, slen);
	*len += slen;
	(*buf)[*len] = '\0';
	return 0;
}

/** @brief send_autoindex function. */
static int
send_autoindex(http_request_t *req, const char *req_path, const char *fullpath)
{
	DIR *dir;
	struct dirent *de;
	char *html;
	char *escaped;
	size_t cap = 4096;
	size_t len = 0;

	dir = opendir(fullpath);
	if (!dir)
		return http_send_error(req, 403, "Directory listing disabled");

	html = malloc(cap);
	if (!html) {
		closedir(dir);
		return http_send_error(req, 500, "Out of memory");
	}
	html[0] = '\0';

	escaped = html_escape(req_path);
	if (!escaped) {
		closedir(dir);
		free(html);
		return http_send_error(req, 500, "Out of memory");
	}

	if (append_str(&html, &cap, &len,
	    "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
	    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	    "<title>Index of ") != 0 ||
	    append_str(&html, &cap, &len, escaped) != 0 ||
	    append_str(&html, &cap, &len,
	    "</title><link rel=\"stylesheet\" href=\"/static/css/custom.css\">"
	    "</head><body><div class=\"container\"><h1>Index of ") != 0 ||
	    append_str(&html, &cap, &len, escaped) != 0 ||
	    append_str(&html, &cap, &len, "</h1><ul>") != 0) {
		free(escaped);
		closedir(dir);
		free(html);
		return http_send_error(req, 500, "Out of memory");
	}
	free(escaped);

	if (strcmp(req_path, "/static/") != 0) {
		if (append_str(&html, &cap, &len,
		    "<li><a href=\"../\">../</a></li>") != 0) {
			closedir(dir);
			free(html);
			return http_send_error(req, 500, "Out of memory");
		}
	}

	while ((de = readdir(dir)) != NULL) {
		char item[1024];
		int is_dir = 0;
		struct stat st;
		char pbuf[1024];
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		snprintf(pbuf, sizeof(pbuf), "%s/%s", fullpath, de->d_name);
		if (stat(pbuf, &st) == 0 && S_ISDIR(st.st_mode))
			is_dir = 1;
		escaped = html_escape(de->d_name);
		if (!escaped)
			continue;
		snprintf(item, sizeof(item), "<li><a href=\"%s%s\">%s%s</a></li>",
		    escaped, is_dir ? "/" : "", escaped, is_dir ? "/" : "");
		free(escaped);
		if (append_str(&html, &cap, &len, item) != 0) {
			closedir(dir);
			free(html);
			return http_send_error(req, 500, "Out of memory");
		}
	}
	closedir(dir);

	if (append_str(&html, &cap, &len, "</ul></div></body></html>") != 0) {
		free(html);
		return http_send_error(req, 500, "Out of memory");
	}

	http_response_t *resp = http_response_create();
	if (!resp) {
		free(html);
		return -1;
	}
	resp->content_type = "text/html; charset=utf-8";
	http_response_set_body(resp, html, len, 1);
	int ret = http_response_send(req, resp);
	http_response_free(resp);
	return ret;
}

/** @brief send_redirect function. */
static int
send_redirect(http_request_t *req, int status_code, const char *location)
{
	http_response_t *resp = http_response_create();
	if (!resp)
		return -1;
	http_response_set_status(resp, status_code);
	resp->content_type = "text/plain; charset=utf-8";
	http_response_add_header(resp, "Location", location);
	http_response_set_body(resp, "", 0, 0);
	int ret = http_response_send(req, resp);
	http_response_free(resp);
	return ret;
}

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
	char fullpath[512];
	char req_path[512];
	char redirect[512];
	char index_path[512];
	const char *url;
	const char *relpath;
	const char *mime;
	struct stat st;
	size_t req_path_len;
	int n;

	if (!req || !req->url)
		return http_send_error(req, 400, "Bad Request");

	/* Strip query/fragment from URL so /static/app.js?v=1 resolves normally. */
	url = req->url;
	req_path_len = strcspn(url, "?#");
	if (req_path_len == 0 || req_path_len >= sizeof(req_path))
		return http_send_error(req, 400, "Bad Request");
	memcpy(req_path, url, req_path_len);
	req_path[req_path_len] = '\0';

	if (strncmp(req_path, "/static", 7) != 0)
		return http_send_error(req, 404, "Not Found");

	relpath = req_path + 7;
	if (*relpath == '/')
		relpath++;
	if (*relpath == '\0')
		relpath = ".";

	/* Prevent path traversal and malformed duplicate separators. */
	if (strstr(relpath, "..") || strstr(relpath, "//"))
		return http_send_error(req, 403, "Forbidden");

	n = snprintf(fullpath, sizeof(fullpath), "%s/%s", config_static_dir,
	    relpath);
	if (n < 0 || (size_t)n >= sizeof(fullpath))
		return http_send_error(req, 414, "URI Too Long");

	if (stat(fullpath, &st) != 0)
		return http_send_error(req, 404, "File not found");

	if (S_ISDIR(st.st_mode)) {
		if (!config_autoindex)
			return http_send_error(req, 403, "Directory listing disabled");

		/* Canonical directory URL with trailing slash keeps relative assets valid. */
		if (req_path[req_path_len - 1] != '/') {
			n = snprintf(redirect, sizeof(redirect), "%s/", req_path);
			if (n < 0 || (size_t)n >= sizeof(redirect))
				return http_send_error(req, 414, "URI Too Long");
			return send_redirect(req, 301, redirect);
		}

		n = snprintf(index_path, sizeof(index_path), "%s/index.html", fullpath);
		if (n > 0 && (size_t)n < sizeof(index_path) && access(index_path, R_OK) == 0)
			return http_send_file(req, index_path, mime_type_for_path("index.html"));

		n = snprintf(index_path, sizeof(index_path), "%s/index.htm", fullpath);
		if (n > 0 && (size_t)n < sizeof(index_path) && access(index_path, R_OK) == 0)
			return http_send_file(req, index_path, mime_type_for_path("index.htm"));

		return send_autoindex(req, req_path, fullpath);
	}

	mime = mime_type_for_path(relpath);
	return http_send_file(req, fullpath, mime);
 }
