#include <string.h>
#include "../include/urls.h"
#include "../include/man.h"
#include "../include/metrics.h"

static struct route routes[MAX_ROUTES];
static size_t route_count = 0;

void
register_route(const char *method, const char *path, route_handler_t handler, void *handler_cls)
{
    if (route_count < MAX_ROUTES) {
        routes[route_count].method = method;
        routes[route_count].path = path;
        routes[route_count].handler = handler;
        routes[route_count].handler_cls = handler_cls;
        route_count++;
    }
}

void
init_routes(void)
{
    /* Registrazione rotte statiche */
    register_route("GET", "/", dashboard_handler, NULL);
    register_route("GET", "/docs", man_handler, NULL);
    register_route("GET", "/favicon.ico", favicon_handler, NULL);
    register_route("GET", "/api/metrics", metrics_handler, NULL);
    register_route("GET", "/apiroot", apiroot_handler, NULL);
}

route_handler_t
find_route_match(const char *method, const char *path)
{
    /* 1. Controllo rotte esatte registrate */
    for (size_t i = 0; i < route_count; i++) {
        if (strcmp(routes[i].method, method) == 0 &&
            strcmp(routes[i].path, path) == 0) {
            return routes[i].handler;
            }
    }

    /* 2. Logica per rotte dinamiche (GET) */
    if (strcmp(method, "GET") == 0) {
        if (strncmp(path, "/man/", 5) == 0) {
            const char *p = path + 5;
            int slash_count = 0;
            while (*p) if (*p++ == '/') slash_count++;
            if (slash_count >= 2) return man_render_handler;
        }

        if (strncmp(path, "/api/man", 8) == 0) return man_api_handler;
        if (strncmp(path, "/static/", 8) == 0) return static_handler;
    }

    return NULL;
}
