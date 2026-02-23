#include "../../include/miniweb/router/router.h"

/**
 * @brief Register a route through the router callback interface.
 * @param r Router instance with callback vtable and context.
 * @param method HTTP method string (for example "GET").
 * @param path Route path pattern to register.
 * @param handler Function invoked when the route matches.
 * @return 0 on success, -1 when the router interface is invalid.
 */
int
router_register(struct router *r, const char *method,
	const char *path, route_handler_t handler)
{
	if (!r || !r->register_fn)
		return -1;
	return r->register_fn(r->ctx, method, path, handler);
}
