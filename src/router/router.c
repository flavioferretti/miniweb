/* router.c - minimal routing */
#include <miniweb/router/router.h>

/**
 * @brief TODO: Describe router_register.
 * @param r TODO: Describe this parameter.
 * @param method TODO: Describe this parameter.
 * @param path TODO: Describe this parameter.
 * @param handler TODO: Describe this parameter.
 * @return TODO: Describe the return value.
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
 * @brief TODO: Describe router_register_prefix.
 * @param r TODO: Describe this parameter.
 * @param method TODO: Describe this parameter.
 * @param prefix TODO: Describe this parameter.
 * @param min_slashes TODO: Describe this parameter.
 * @param handler TODO: Describe this parameter.
 * @return TODO: Describe the return value.
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
