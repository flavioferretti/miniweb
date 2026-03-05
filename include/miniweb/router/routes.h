/* routes.h - Route mapping for native kqueue engine */

#ifndef MINIWEB_ROUTER_ROUTES_H
#define MINIWEB_ROUTER_ROUTES_H

#include <miniweb/http/handler.h>

struct router;

/* route_handler_t is an alias for http_handler_t — same signature,
 * one canonical type defined in http_handler.h. */
typedef http_handler_t route_handler_t;

/** Initialize and register all static routes. */
void init_routes(void *module_cfg);

/** Resolve the best handler for an HTTP method/path pair. */
route_handler_t route_match(const char *method, const char *path);

/** Render a template-backed view page from the route table. */
int view_template_handler(http_request_t *req);

/** Serve /favicon.svg content. */
int favicon_handler(http_request_t *req);

/** Serve files under /static/. */
int static_handler(http_request_t *req);

/** Serve /api/metrics JSON payload. */
int metrics_handler(http_request_t *req);

/** Serve /api/networking JSON payload. */
int networking_api_handler(http_request_t *req);

/** Serve rendered man page content. */
int man_render_handler(http_request_t *req);

/** Serve /api/man JSON endpoints. */
int man_api_handler(http_request_t *req);

/** Serve /api/packages JSON endpoints. */
int pkg_api_handler(http_request_t *req);

int views_module_attach_routes(struct router *r);

#endif /* MINIWEB_ROUTER_ROUTES_H */
