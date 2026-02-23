/* module_attach.c - module attach facility */
#include <miniweb/router/module_attach.h>

/**
 * @brief TODO: Describe miniweb_module_attach_enabled.
 * @param r TODO: Describe this parameter.
 * @param modules TODO: Describe this parameter.
 * @param count TODO: Describe this parameter.
 * @param ctx TODO: Describe this parameter.
 * @return TODO: Describe the return value.
 */
/**
 * @brief Initialize and attach every enabled module to the router.
 * @param r Router facade used by module attach callbacks.
 * @param modules Array of module descriptors.
 * @param count Number of module descriptors in @p modules.
 * @param ctx Opaque initialization context forwarded to each module.
 * @return 0 on success, -1 if a module init or route attach callback fails.
 */
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
