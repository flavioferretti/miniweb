#ifndef MAN_H
#define MAN_H

#include <microhttpd.h>

/* Costanti per i limiti dei buffer */
#define MAX_JSON_SIZE (4 * 1024 * 1024)      /* 4MB */
#define MAX_OUTPUT_SIZE 2097152    /* 2MB per pagine renderizzate */

/* Handler per le rotte definiti in routes.c */
int man_api_handler(void *cls, struct MHD_Connection *connection,
                    const char *url, const char *method,
                    const char *version, const char *upload_data,
                    size_t *upload_data_size, void **con_cls);

int man_render_handler(void *cls, struct MHD_Connection *connection,
                       const char *url, const char *method,
                       const char *version, const char *upload_data,
                       size_t *upload_data_size, void **con_cls);

/* Funzioni di core logic */
char* man_get_sections_json(void);
static void json_escape(char *dest, const char *src, size_t max);
char* man_get_section_pages_json(const char *section);
char* man_get_page_metadata_json(const char *section, const char *name);
char* man_render_page(const char *section, const char *name, const char *format);
char* man_search_json(const char *query);

#endif /* MAN_H */
