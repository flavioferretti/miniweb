#include <miniweb/router/router.h>

int
router_register(struct router *r, const char *method,
	const char *path, route_handler_t handler)
{
	if (!r || !r->register_fn)
		return -1;
	return r->register_fn(r->ctx, method, path, handler);
}

int
router_register_prefix(struct router *r, const char *method,
	const char *prefix, int min_slashes, route_handler_t handler)
{
	if (!r || !r->register_prefix_fn)
		return -1;
	return r->register_prefix_fn(r->ctx, method, prefix,
	    min_slashes, handler);
}
