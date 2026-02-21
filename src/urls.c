/* urls.c - URL routing table */

#include <string.h>

#include "../include/man.h"
#include "../include/metrics.h"
#include "../include/networking.h"
#include "../include/urls.h"

static struct route routes[MAX_ROUTES];
static size_t route_count = 0;

static const struct view_route view_routes[] = {
	{"GET", "/", "MiniWeb - Dashboard", "dashboard.html",
	 "dashboard_extra_head.html", "dashboard_extra_js.html"},
	{"GET", "/docs", "MiniWeb - Documentation", "docs.html",
	 "docs_extra_head.html", "docs_extra_js.html"},
	{"GET", "/apiroot", "MiniWeb - API Root", "api.html",
	 "api_extra_head.html", "api_extra_js.html"},
	{"GET", "/networking", "MiniWeb - Networking", "networking.html",
	 "networking_extra_head.html", "networking_extra_js.html"},
	{"GET", "/packages", "MiniWeb - Package Manager", "packages.html",
	 "packages_extra_head.html", "packages_extra_js.html"},
};

/**
 * @brief Register view routes.
 */
static void
register_view_routes(void)
{
	for (size_t i = 0; i < sizeof(view_routes) / sizeof(view_routes[0]); i++) {
		register_route(view_routes[i].method, view_routes[i].path,
				       view_template_handler);
	}
}

/**
 * @brief Register pkg api routes.
 */
static void
register_pkg_api_routes(void)
{
	static const char *const pkg_paths[] = {
		"/api/packages/search",
		"/api/packages/info",
		"/api/packages/which",
		"/api/packages/files",
		"/api/packages/list",
	};

	for (size_t i = 0; i < sizeof(pkg_paths) / sizeof(pkg_paths[0]); i++)
		register_route("GET", pkg_paths[i], pkg_api_handler);
}

/**
 * @brief Register route.
 * @param method HTTP method string.
 * @param path Request or filesystem path to evaluate.
 * @param handler Route handler callback.
 */
void
register_route(const char *method, const char *path, route_handler_t handler)
{
	if (route_count < MAX_ROUTES) {
		routes[route_count].method = method;
		routes[route_count].path = path;
		routes[route_count].handler = handler;
		route_count++;
	}
}

/**
 * @brief Find view route.
 * @param method HTTP method string.
 * @param path Request or filesystem path to evaluate.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
const struct view_route *
find_view_route(const char *method, const char *path)
{
	for (size_t i = 0; i < sizeof(view_routes) / sizeof(view_routes[0]); i++) {
		if (strcmp(view_routes[i].method, method) == 0 &&
			strcmp(view_routes[i].path, path) == 0)
			return &view_routes[i];
	}

	return NULL;
}

/**
 * @brief Init routes.
 */
void
init_routes(void)
{
	register_view_routes();

	register_route("GET", "/favicon.ico", favicon_handler);
	register_route("GET", "/api/metrics", metrics_handler);
	register_route("GET", "/api/networking", networking_api_handler);
	register_pkg_api_routes();
}

/**
 * @brief Route match.
 * @param method HTTP method string.
 * @param path Request or filesystem path to evaluate.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
route_handler_t
route_match(const char *method, const char *path)
{
	/* 1. Exact match */
	for (size_t i = 0; i < route_count; i++) {
		if (strcmp(routes[i].method, method) == 0 &&
		    strcmp(routes[i].path, path) == 0)
			return routes[i].handler;
	}

	/* 2. Dynamic routes (GET only) */
	if (strcmp(method, "GET") == 0) {
		/* /man/{area}/{section}/{page}[.fmt] */
		if (strncmp(path, "/man/", 5) == 0) {
			const char *p = path + 5;
			int slashes = 0;
			while (*p)
				if (*p++ == '/')
					slashes++;
			if (slashes >= 2)
				return man_render_handler;
		}

		/* /api/man/... */
		if (strncmp(path, "/api/man", 8) == 0)
			return man_api_handler;

		/* /api/packages/... */
		if (strncmp(path, "/api/packages", 13) == 0)
			return pkg_api_handler;

		/* /static/... */
		if (strncmp(path, "/static/", 8) == 0)
			return static_handler;
	}

	return NULL;
}
