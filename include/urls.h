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

/* Exported functions */
void            init_routes(void);
route_handler_t find_route_match(const char *method, const char *path);
void            register_route(const char *method, const char *path,
                               route_handler_t handler);

#endif /* URLS_H */
