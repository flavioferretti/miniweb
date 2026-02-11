#ifndef MAN_H
#define MAN_H

#include <microhttpd.h>
#include <stddef.h>

/**
 * Core JSON API Functions
 * Restituiscono stringhe JSON allocate dinamicamente (da liberare con free())
 */
char *man_get_sections_json(void);
char *man_get_section_pages_json(const char *area, const char *section);
char *man_get_page_metadata_json(const char *area, const char *section, const char *name);
char *man_api_search(const char *query);

/**
 * HTTP Handlers
 * Gestiscono le richieste dirette da libmicrohttpd
 */

/* Gestisce il rendering della pagina nei vari formati (.html, .pdf, .md, .ps) */
int man_render_handler(void *cls, struct MHD_Connection *connection,
                       const char *url, const char *method, const char *version,
                       const char *upload_data, size_t *upload_data_size,
                       void **con_cls);

/* Gestisce le chiamate API sotto /api/man/ */
int man_api_handler(void *cls, struct MHD_Connection *connection,
                    const char *url, const char *method, const char *version,
                    const char *upload_data, size_t *upload_data_size,
                    void **con_cls);

#endif /* MAN_H */
