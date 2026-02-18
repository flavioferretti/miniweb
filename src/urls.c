/* urls.c - URL routing table */

#include <string.h>
#include "../include/urls.h"
#include "../include/man.h"
#include "../include/metrics.h"
#include "../include/networking.h"

static struct route routes[MAX_ROUTES];
static size_t       route_count = 0;

void
register_route(const char *method, const char *path, route_handler_t handler)
{
    if (route_count < MAX_ROUTES) {
        routes[route_count].method  = method;
        routes[route_count].path    = path;
        routes[route_count].handler = handler;
        route_count++;
    }
}

void
init_routes(void)
{
    register_route("GET", "/",               dashboard_handler);
    register_route("GET", "/docs",           man_handler);
    register_route("GET", "/favicon.ico",    favicon_handler);
    register_route("GET", "/api/metrics",    metrics_handler);
    register_route("GET", "/apiroot",        apiroot_handler);
    register_route("GET", "/networking",     networking_handler);
    register_route("GET", "/api/networking", networking_api_handler);
}

route_handler_t
find_route_match(const char *method, const char *path)
{
    /* 1. Exact match */
    for (size_t i = 0; i < route_count; i++) {
        if (strcmp(routes[i].method, method) == 0 &&
            strcmp(routes[i].path, path) == 0)
            return routes[i].handler;
    }

    /* 2. Dynamic routes (GET only) */
    if (strcmp(method, "GET") == 0) {
        /* /man/{area}/{section}/{page}[.fmt] */
        if (strncmp(path, "/man/", 5) == 0) {
            const char *p = path + 5;
            int slashes = 0;
            while (*p) if (*p++ == '/') slashes++;
            if (slashes >= 2) return man_render_handler;
        }

        /* /api/man/... */
        if (strncmp(path, "/api/man", 8) == 0)
            return man_api_handler;

        /* /static/... */
        if (strncmp(path, "/static/", 8) == 0)
            return static_handler;
    }

    return NULL;
}
