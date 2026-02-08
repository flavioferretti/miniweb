/* metrics.h - System metrics header */
#ifndef METRICS_H
#define METRICS_H

#include <microhttpd.h>

char* get_system_metrics_json(void);
int metrics_handler(void *cls, struct MHD_Connection *connection,
                    const char *url, const char *method,
                    const char *version, const char *upload_data,
                    size_t *upload_data_size, void **con_cls);

#endif /* METRICS_H */
