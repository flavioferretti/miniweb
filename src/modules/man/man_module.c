#include <miniweb/modules/man.h>
#include "man_internal.h"
#include <miniweb/router/router.h>

void
man_module_cleanup(void)
{
    man_render_cache_cleanup();
}

int
man_module_attach_routes(struct router *r)
{
    if (router_register_prefix(r, "GET", "/man/", 2, man_render_handler) != 0)
        return -1;
    if (router_register_prefix(r, "GET", "/api/man", 0, man_api_handler) != 0)
        return -1;
    if (router_register(r, "GET", "/api/man/sections", man_api_handler) != 0)
        return -1;
    return 0;
}
