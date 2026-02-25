
/* router.c - minimal routing */
#include <miniweb/router/router.h>

/**
 * @brief Register an exact-match route with the router.
 * @param r       Router instance.
 * @param method  HTTP method string.
 * @param path    Exact URL path.
 * @param handler Route handler function.
 * @return 0 on success, -1 on overflow or invalid input.
 */
int
router_register(struct router *r, const char *method,
				const char *path, route_handler_t handler)
{
	if (!r || !r->register_fn)
		return -1;
	return r->register_fn(r->ctx, method, path, handler);
}

/**
 * @brief Register a prefix-match route with the router.
 * @param r         Router instance.
 * @param method    HTTP method string.
 * @param prefix    URL prefix to match.
 * @param min_extra Minimum additional path characters required after prefix.
 * @param handler   Route handler function.
 * @return 0 on success, -1 on overflow or invalid input.
 */
int
router_register_prefix(struct router *r, const char *method,
					   const char *prefix, int min_slashes, route_handler_t handler)
{
	if (!r || !r->register_prefix_fn)
		return -1;
	return r->register_prefix_fn(r->ctx, method, prefix,
								 min_slashes, handler);
}
