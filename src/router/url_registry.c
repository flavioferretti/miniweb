/* url_registry.c - URL routing table */

#include <string.h>

#include <miniweb/router/urls.h>

#include "url_registry_internal.h"

struct route routes[MAX_ROUTES];
size_t route_count = 0;
struct prefix_route prefix_routes[MAX_PREFIX_ROUTES];
size_t prefix_route_count = 0;

/**
 * @brief path_slash_count operation.
 *
 * @details Performs the core path_slash_count routine for this module.
 *
 * @param path Input parameter for path_slash_count.
 *
 * @return Return value produced by path_slash_count.
 */
int
path_slash_count(const char *path)
{
	int slashes = 0;

	while (*path)
		if (*path++ == '/')
			slashes++;
	return slashes;
}

/**
 * @brief prefix_route_matches operation.
 *
 * @details Performs the core prefix_route_matches routine for this module.
 *
 * @param pr Input parameter for prefix_route_matches.
 * @param method Input parameter for prefix_route_matches.
 * @param path Input parameter for prefix_route_matches.
 *
 * @return Return value produced by prefix_route_matches.
 */
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

/**
 * @brief prefix_route_path_matches operation.
 *
 * @details Performs the core prefix_route_path_matches routine for this module.
 *
 * @param pr Input parameter for prefix_route_path_matches.
 * @param path Input parameter for prefix_route_path_matches.
 *
 * @return Return value produced by prefix_route_path_matches.
 */
int
prefix_route_path_matches(const struct prefix_route *pr, const char *path)
{
	if (strncmp(path, pr->prefix, strlen(pr->prefix)) != 0)
		return 0;
	if (pr->min_slashes <= 0)
		return 1;

	return path_slash_count(path + strlen(pr->prefix)) >= pr->min_slashes;
}

/**
 * @brief route_method_seen_for_path operation.
 *
 * @details Performs the core route_method_seen_for_path routine for this module.
 *
 * @param route_index Input parameter for route_method_seen_for_path.
 * @param path Input parameter for route_method_seen_for_path.
 *
 * @return Return value produced by route_method_seen_for_path.
 */
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

/**
 * @brief register_route operation.
 *
 * @details Performs the core register_route routine for this module.
 *
 * @param method Input parameter for register_route.
 * @param path Input parameter for register_route.
 * @param handler Input parameter for register_route.
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
 * @brief register_prefix_route operation.
 *
 * @details Performs the core register_prefix_route routine for this module.
 *
 * @param method Input parameter for register_prefix_route.
 * @param prefix Input parameter for register_prefix_route.
 * @param min_slashes Input parameter for register_prefix_route.
 * @param handler Input parameter for register_prefix_route.
 */
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
