#ifndef MINIWEB_ROUTER_MODULE_ATTACH_H
#define MINIWEB_ROUTER_MODULE_ATTACH_H

#include <stddef.h>

#include "router.h"

struct miniweb_module {
	const char *name;
	int (*init)(void *ctx);
	int (*attach_routes)(struct router *r);
	void (*shutdown)(void *ctx);
	int enabled_by_default;
};

int miniweb_module_attach_enabled(struct router *r,
	struct miniweb_module *modules, size_t count, void *ctx);

#endif
