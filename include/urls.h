#ifndef URLS_H
#define URLS_H

#include <stddef.h>
#include "routes.h"

#define MAX_ROUTES 32

/* Struttura interna per memorizzare le rotte */
struct route {
    const char *method;
    const char *path;
    route_handler_t handler;
    void *handler_cls;
};

/* Funzioni esportate */
void init_routes(void);
route_handler_t find_route_match(const char *method, const char *path);
void register_route(const char *method, const char *path, route_handler_t handler, void *handler_cls);

#endif
