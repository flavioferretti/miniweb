/* urls.c - URL routing table */

#include <stdio.h>
#include <string.h>

#include "../../include/man.h"
#include "../../include/metrics.h"
#include "../../include/networking.h"
#include "../../include/pkg_manager.h"
#include "../../include/routes.h"
#include "../../include/miniweb/router/module_attach.h"
#include "../../include/urls.h"

static struct route routes[MAX_ROUTES];
static size_t route_count = 0;

#define MAX_PREFIX_ROUTES 16
static struct prefix_route prefix_routes[MAX_PREFIX_ROUTES];
static size_t prefix_route_count = 0;

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
url_registry_register_prefix(void *ctx, const char *method,
	const char *prefix, int min_slashes, route_handler_t handler)
{
	(void)ctx;
	register_prefix_route(method, prefix, min_slashes, handler);
	return 0;
}

int
views_module_attach_routes(struct router *r)
{
	for (size_t i = 0; i < sizeof(view_routes) / sizeof(view_routes[0]); i++) {
		if (router_register(r, view_routes[i].method,
		    view_routes[i].path, view_template_handler) != 0)
			return -1;
	}
	if (router_register(r, "GET", "/favicon.ico", favicon_handler) != 0)
		return -1;
	if (router_register_prefix(r, "GET", "/static/", 0,
	    static_handler) != 0)
		return -1;
	return 0;
}

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

void
register_prefix_route(const char *method, const char *prefix,
	int min_slashes, route_handler_t handler)
{
	if (prefix_route_count >= MAX_PREFIX_ROUTES)
		return;

	prefix_routes[prefix_route_count].method = method;
	prefix_routes[prefix_route_count].prefix = prefix;
	prefix_routes[prefix_route_count].min_slashes = min_slashes;
	prefix_routes[prefix_route_count].handler = handler;
	prefix_route_count++;
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
		{
			.name = "views",
			.attach_routes = views_module_attach_routes,
			.enabled_by_default = 1,
		},
		{
			.name = "metrics",
			.attach_routes = metrics_module_attach_routes,
			.enabled_by_default = 1,
		},
		{
			.name = "networking",
			.attach_routes = networking_module_attach_routes,
			.enabled_by_default = 1,
		},
		{
			.name = "man",
			.attach_routes = man_module_attach_routes,
			.enabled_by_default = 1,
		},
		{
			.name = "packages",
			.attach_routes = packages_module_attach_routes,
			.enabled_by_default = 1,
		},
	};

	route_count = 0;
	prefix_route_count = 0;

	(void)miniweb_module_attach_enabled(&r, modules,
	    sizeof(modules) / sizeof(modules[0]), NULL);
}

route_handler_t
route_match(const char *method, const char *path)
{
	for (size_t i = 0; i < route_count; i++) {
		if (strcmp(routes[i].method, method) == 0 &&
		    strcmp(routes[i].path, path) == 0)
			return routes[i].handler;
	}

	for (size_t i = 0; i < prefix_route_count; i++) {
		const struct prefix_route *pr = &prefix_routes[i];
		const char *p;
		int slashes = 0;

		if (strcmp(pr->method, method) != 0)
			continue;
		if (strncmp(path, pr->prefix, strlen(pr->prefix)) != 0)
			continue;
		if (pr->min_slashes <= 0)
			return pr->handler;

		p = path + strlen(pr->prefix);
		while (*p)
			if (*p++ == '/')
				slashes++;
		if (slashes >= pr->min_slashes)
			return pr->handler;
	}

	return NULL;
}

int
route_allow_methods(const char *path, char *buf, size_t buf_len)
{
	if (!path || !buf || buf_len == 0)
		return 0;

	buf[0] = '\0';
	size_t used = 0;
	int count = 0;

	for (size_t i = 0; i < route_count; i++) {
		if (strcmp(routes[i].path, path) != 0)
			continue;

		int seen = 0;
		for (size_t j = 0; j < i; j++) {
			if (strcmp(routes[j].path, path) == 0 &&
			    strcmp(routes[j].method, routes[i].method) == 0) {
				seen = 1;
				break;
			}
		}
		if (seen)
			continue;

		int wrote = snprintf(buf + used, buf_len - used,
		    "%s%s", count > 0 ? ", " : "", routes[i].method);
		if (wrote < 0 || (size_t)wrote >= buf_len - used)
			return count;

		used += (size_t)wrote;
		count++;
	}

	for (size_t i = 0; i < prefix_route_count; i++) {
		if (strncmp(path, prefix_routes[i].prefix,
		    strlen(prefix_routes[i].prefix)) != 0)
			continue;
		if (count == 0 && buf_len > 3)
			(void)snprintf(buf, buf_len, "%s", prefix_routes[i].method);
		if (count == 0)
			count = 1;
		break;
	}

	return count;
}

int
route_path_known(const char *path)
{
	if (!path)
		return 0;

	for (size_t i = 0; i < route_count; i++) {
		if (strcmp(routes[i].path, path) == 0)
			return 1;
	}

	for (size_t i = 0; i < prefix_route_count; i++) {
		if (strncmp(path, prefix_routes[i].prefix,
		    strlen(prefix_routes[i].prefix)) != 0)
			continue;
		if (prefix_routes[i].min_slashes <= 0)
			return 1;

		const char *p = path + strlen(prefix_routes[i].prefix);
		int slashes = 0;
		while (*p)
			if (*p++ == '/')
				slashes++;
		if (slashes >= prefix_routes[i].min_slashes)
			return 1;
	}

	return 0;
}
