#ifndef URLS_H
#define URLS_H

#include <stddef.h>
#include "routes.h"

#define MAX_ROUTES 32

/* Internal route table entry */
struct route {
    const char    *method;
    const char    *path;
    route_handler_t handler;
    /* handler_cls removed: new handler signature is handler(req),
     * per-handler context is not needed. */
};

/* Declarative template-backed view route. */
struct view_route {
	const char *method;
	const char *path;
	const char *title;
	const char *page;
	const char *extra_head;
	const char *extra_js;
};

/** Initialize static route registrations. */
void init_routes(void);

/** Return the matching handler for method/path, or NULL when not found. */
route_handler_t route_match(const char *method, const char *path);

/** Register one method/path to handler mapping into the route table. */
void register_route(const char *method, const char *path,
                               route_handler_t handler);

/** Find a declarative view route by method/path, or NULL if not found. */
const struct view_route *find_view_route(const char *method, const char *path);

#endif /* URLS_H */
