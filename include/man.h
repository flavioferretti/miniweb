/* man.h - Manual pages API interface */

#ifndef MAN_H
#define MAN_H

#include <microhttpd.h>

#define MAX_JSON_SIZE 1048576  /* 1MB */
#define MAX_OUTPUT_SIZE 2097152  /* 2MB for rendered pages */

/* Route handlers */
int man_api_handler(void *cls, struct MHD_Connection *connection,
                    const char *url, const char *method,
                    const char *version, const char *upload_data,
                    size_t *upload_data_size, void **con_cls);

int man_render_handler(void *cls, struct MHD_Connection *connection,
                       const char *url, const char *method,
                       const char *version, const char *upload_data,
                       size_t *upload_data_size, void **con_cls);

/* Helper functions - Updated with area parameter */
char* man_get_sections_json(void);
char* man_get_section_pages_json(const char *area, const char *section);
char* man_get_page_metadata_json(const char *area, const char *section, const char *name);
char* man_render_page(const char *area, const char *section, const char *name, const char *format);
char* man_search_json(const char *query);

#endif /* MAN_H */
