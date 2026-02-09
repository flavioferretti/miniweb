/* routes.h */

#ifndef ROUTES_H
#define ROUTES_H

#include <microhttpd.h>

/* Handler function type */
typedef int (*route_handler_t)(void *cls, struct MHD_Connection *connection,
                               const char *url, const char *method,
                               const char *version, const char *upload_data,
                               size_t *upload_data_size, void **con_cls);

/* Route structure */
struct route {
    const char *method;
    const char *path;
    route_handler_t handler;
    void *handler_cls;
};

/* Route matching function */
route_handler_t route_match(const char *method, const char *path);

/* Initialize routes */
void init_routes(void);

/* Built-in handlers */
int man_handler(void *cls, struct MHD_Connection *connection,
                  const char *url, const char *method,
                  const char *version, const char *upload_data,
                  size_t *upload_data_size, void **con_cls);

int dashboard_handler(void *cls, struct MHD_Connection *connection,
                 const char *url, const char *method,
                 const char *version, const char *upload_data,
                 size_t *upload_data_size, void **con_cls);

int favicon_handler(void *cls, struct MHD_Connection *connection,
                    const char *url, const char *method,
                    const char *version, const char *upload_data,
                    size_t *upload_data_size, void **con_cls);

int static_handler(void *cls, struct MHD_Connection *connection,
                   const char *url, const char *method,
                   const char *version, const char *upload_data,
                   size_t *upload_data_size, void **con_cls);

/* Metrics API handler */
int metrics_handler(void *cls, struct MHD_Connection *connection,
                    const char *url, const char *method,
                    const char *version, const char *upload_data,
                    size_t *upload_data_size, void **con_cls);

/* Helper functions for metrics */
char* get_system_metrics_json(void);
char* get_cpu_info(void);
char* get_memory_info(void);
char* get_network_info(void);
char* get_disk_info(void);
char* get_uptime_info(void);

/* Man pages handlers (from man.c) */
int man_api_handler(void *cls, struct MHD_Connection *connection,
                    const char *url, const char *method,
                    const char *version, const char *upload_data,
                    size_t *upload_data_size, void **con_cls);

int man_render_handler(void *cls, struct MHD_Connection *connection,
                       const char *url, const char *method,
                       const char *version, const char *upload_data,
                       size_t *upload_data_size, void **con_cls);

#endif
