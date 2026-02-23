#include "../../include/miniweb/router/router.h"

int
router_register(struct router *r, const char *method,
	const char *path, route_handler_t handler)
{
	if (!r || !r->register_fn)
		return -1;
	return r->register_fn(r->ctx, method, path, handler);
}
