/* url_registry.c - URL routing table */

#include <string.h>

#include <miniweb/router/urls.h>

#include "url_registry_internal.h"

struct route routes[MAX_ROUTES];
size_t route_count = 0;
struct prefix_route prefix_routes[MAX_PREFIX_ROUTES];
size_t prefix_route_count = 0;

/** @brief path_slash_count function. */
int
path_slash_count(const char *path)
{
	int slashes = 0;

	while (*path)
		if (*path++ == '/')
			slashes++;
	return slashes;
}

/** @brief prefix_route_matches function. */
int
prefix_route_matches(const struct prefix_route *pr, const char *method,
	const char *path)
{
	if (strcmp(pr->method, method) != 0)
		return 0;
	if (strncmp(path, pr->prefix, strlen(pr->prefix)) != 0)
		return 0;
	if (pr->min_slashes <= 0)
		return 1;

	return path_slash_count(path + strlen(pr->prefix)) >= pr->min_slashes;
}

/** @brief prefix_route_path_matches function. */
int
prefix_route_path_matches(const struct prefix_route *pr, const char *path)
{
	if (strncmp(path, pr->prefix, strlen(pr->prefix)) != 0)
		return 0;
	if (pr->min_slashes <= 0)
		return 1;

	return path_slash_count(path + strlen(pr->prefix)) >= pr->min_slashes;
}

/** @brief route_method_seen_for_path function. */
int
route_method_seen_for_path(size_t route_index, const char *path)
{
	for (size_t j = 0; j < route_index; j++) {
		if (strcmp(routes[j].path, path) == 0 &&
			strcmp(routes[j].method, routes[route_index].method) == 0)
			return 1;
	}
	return 0;
}

/** @brief register_route function. */
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

/** @brief register_prefix_route function. */
void
register_prefix_route(const char *method, const char *prefix, int min_slashes,
	route_handler_t handler)
{
	if (prefix_route_count >= MAX_PREFIX_ROUTES)
		return;

	prefix_routes[prefix_route_count].method = method;
	prefix_routes[prefix_route_count].prefix = prefix;
	prefix_routes[prefix_route_count].min_slashes = min_slashes;
	prefix_routes[prefix_route_count].handler = handler;
	prefix_route_count++;
}
