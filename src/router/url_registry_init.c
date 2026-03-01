#include <string.h>

#include <miniweb/modules/man.h>
#include <miniweb/modules/metrics.h>
#include <miniweb/modules/networking.h>
#include <miniweb/modules/pkg_manager.h>
#include <miniweb/router/module_attach.h>
#include <miniweb/router/routes.h>
#include <miniweb/router/urls.h>

#include "url_registry_internal.h"

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

static int
url_registry_register(void *ctx, const char *method, const char *path,
	route_handler_t handler)
{
	(void)ctx;
	register_route(method, path, handler);
	return 0;
}

static int
url_registry_register_prefix(void *ctx, const char *method, const char *prefix,
	int min_slashes, route_handler_t handler)
{
	(void)ctx;
	register_prefix_route(method, prefix, min_slashes, handler);
	return 0;
}

int
views_module_attach_routes(struct router *r)
{
	for (size_t i = 0; i < sizeof(view_routes) / sizeof(view_routes[0]); i++) {
		if (router_register(r, view_routes[i].method, view_routes[i].path,
			view_template_handler) != 0)
			return -1;
	}
	if (router_register(r, "GET", "/favicon.ico", favicon_handler) != 0)
		return -1;
	if (router_register_prefix(r, "GET", "/static/", 0, static_handler) != 0)
		return -1;
	return 0;
}

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

void
init_routes(void)
{
	struct router r = {
		.register_fn = url_registry_register,
		.register_prefix_fn = url_registry_register_prefix,
		.ctx = NULL,
	};
	struct miniweb_module modules[] = {
		{ .name = "views", .attach_routes = views_module_attach_routes,
			.enabled_by_default = 1 },
		{ .name = "metrics", .attach_routes = metrics_module_attach_routes,
			.enabled_by_default = 1 },
		{ .name = "networking", .attach_routes = networking_module_attach_routes,
			.enabled_by_default = 1 },
		{ .name = "man", .attach_routes = man_module_attach_routes,
			.enabled_by_default = 1 },
		{ .name = "packages", .attach_routes = packages_module_attach_routes,
			.enabled_by_default = 1 },
	};

	route_count = 0;
	prefix_route_count = 0;

	(void)miniweb_module_attach_enabled(
		&r, modules, sizeof(modules) / sizeof(modules[0]), NULL);
}
