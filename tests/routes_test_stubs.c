#include <miniweb/http/handler.h>
#include <miniweb/router/router.h>

int
metrics_handler(http_request_t *req)
{
	(void)req;
	return 0;
}

int
networking_api_handler(http_request_t *req)
{
	(void)req;
	return 0;
}

int
man_api_handler(http_request_t *req)
{
	(void)req;
	return 0;
}

int
man_render_handler(http_request_t *req)
{
	(void)req;
	return 0;
}

int
pkg_api_handler(http_request_t *req)
{
	(void)req;
	return 0;
}

int
metrics_module_attach_routes(struct router *r)
{
	return router_register(r, "GET", "/api/metrics", metrics_handler);
}

int
networking_module_attach_routes(struct router *r)
{
	if (router_register(r, "GET", "/networking", view_template_handler) != 0)
		return -1;
	return router_register(r, "GET", "/api/networking",
	    networking_api_handler);
}

int
man_module_attach_routes(struct router *r)
{
	if (router_register_prefix(r, "GET", "/api/man/", 0, man_api_handler) != 0)
		return -1;
	return router_register_prefix(r, "GET", "/man/", 2,
	    man_render_handler);
}

int
packages_module_attach_routes(struct router *r)
{
	if (router_register(r, "GET", "/packages", view_template_handler) != 0)
		return -1;
	if (router_register(r, "GET", "/api/packages/search", pkg_api_handler) != 0)
		return -1;
	if (router_register(r, "GET", "/api/packages/info", pkg_api_handler) != 0)
		return -1;
	if (router_register(r, "GET", "/api/packages/which", pkg_api_handler) != 0)
		return -1;
	if (router_register(r, "GET", "/api/packages/files", pkg_api_handler) != 0)
		return -1;
	return router_register(r, "GET", "/api/packages/list", pkg_api_handler);
}
