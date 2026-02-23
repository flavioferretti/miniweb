#ifndef MINIWEB_ROUTER_ROUTER_H
#define MINIWEB_ROUTER_ROUTER_H

#include <stddef.h>

#include "../../routes.h"

struct router {
	int (*register_fn)(void *ctx, const char *method, const char *path,
		route_handler_t handler);
	void *ctx;
};

int router_register(struct router *r, const char *method,
	const char *path, route_handler_t handler);

#endif
