#include <string.h>

#include <miniweb/router/urls.h>

#include "url_registry_internal.h"

/**
 * @brief route_match operation.
 *
 * @details Performs the core route_match routine for this module.
 *
 * @param method Input parameter for route_match.
 * @param path Input parameter for route_match.
 *
 * @return Return value produced by route_match.
 */
route_handler_t
route_match(const char *method, const char *path)
{
	for (size_t i = 0; i < route_count; i++) {
		if (strcmp(routes[i].method, method) == 0 &&
			strcmp(routes[i].path, path) == 0)
			return routes[i].handler;
	}

	for (size_t i = 0; i < prefix_route_count; i++) {
		if (prefix_route_matches(&prefix_routes[i], method, path))
			return prefix_routes[i].handler;
	}

	return NULL;
}

/**
 * @brief route_path_known operation.
 *
 * @details Performs the core route_path_known routine for this module.
 *
 * @param path Input parameter for route_path_known.
 *
 * @return Return value produced by route_path_known.
 */
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
		if (prefix_route_path_matches(&prefix_routes[i], path))
			return 1;
	}

	return 0;
}
