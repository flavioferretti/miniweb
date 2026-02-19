/* routes.h - Route mapping for native kqueue engine */

#ifndef ROUTES_H
#define ROUTES_H

#include "http_handler.h"

/* route_handler_t is an alias for http_handler_t — same signature,
 * one canonical type defined in http_handler.h. */
typedef http_handler_t route_handler_t;

/* ── Route management ───────────────────────────────────────────── */
void            init_routes(void);
route_handler_t route_match(const char *method, const char *path);

/* ── Handler declarations ───────────────────────────────────────── */
int view_template_handler(http_request_t *req);
int favicon_handler       (http_request_t *req);
int static_handler        (http_request_t *req);
int metrics_handler       (http_request_t *req);
int networking_api_handler(http_request_t *req);
int man_render_handler    (http_request_t *req);
int man_api_handler       (http_request_t *req);

int render_template_response(http_request_t *req, struct template_data *data);

#endif /* ROUTES_H */
