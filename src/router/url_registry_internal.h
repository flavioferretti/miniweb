#ifndef MINIWEB_ROUTER_URL_REGISTRY_INTERNAL_H
#define MINIWEB_ROUTER_URL_REGISTRY_INTERNAL_H

#include <miniweb/router/urls.h>

#define MAX_PREFIX_ROUTES 16

extern struct route routes[MAX_ROUTES];
extern size_t route_count;
extern struct prefix_route prefix_routes[MAX_PREFIX_ROUTES];
extern size_t prefix_route_count;

int path_slash_count(const char *path);
int prefix_route_matches(const struct prefix_route *pr, const char *method,
	const char *path);
int prefix_route_path_matches(const struct prefix_route *pr, const char *path);
int route_method_seen_for_path(size_t route_index, const char *path);

#endif
