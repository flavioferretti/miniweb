#ifndef ROUTES_H
#define ROUTES_H

#include "http_utils.h"
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

/* Route matching */
route_handler_t route_match(const char *method, const char *path);

/* Route initialization */
void init_routes(void);

/* Handler declarations */
int dashboard_handler(void *cls, struct MHD_Connection *connection,
		      const char *url, const char *method, const char *version,
		      const char *upload_data, size_t *upload_data_size,
		      void **con_cls);

int man_handler(void *cls, struct MHD_Connection *connection, const char *url,
		const char *method, const char *version,
		const char *upload_data, size_t *upload_data_size,
		void **con_cls);

int favicon_handler(void *cls, struct MHD_Connection *connection,
		    const char *url, const char *method, const char *version,
		    const char *upload_data, size_t *upload_data_size,
		    void **con_cls);

int static_handler(void *cls, struct MHD_Connection *connection,
		   const char *url, const char *method, const char *version,
		   const char *upload_data, size_t *upload_data_size,
		   void **con_cls);

int metrics_handler(void *cls, struct MHD_Connection *connection,
		    const char *url, const char *method, const char *version,
		    const char *upload_data, size_t *upload_data_size,
		    void **con_cls);

/* Man handlers (declared in man.h) */
#include "man.h"

#endif
