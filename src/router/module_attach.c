#include "../../include/miniweb/router/module_attach.h"

int
miniweb_module_attach_enabled(struct router *r,
	struct miniweb_module *modules, size_t count, void *ctx)
{
	for (size_t i = 0; i < count; i++) {
		if (!modules[i].enabled_by_default)
			continue;
		if (modules[i].init && modules[i].init(ctx) != 0)
			return -1;
		if (modules[i].attach_routes && modules[i].attach_routes(r) != 0)
			return -1;
	}
	return 0;
}
