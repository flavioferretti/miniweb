#ifndef MAN_H
#define MAN_H

#include <microhttpd.h>

/* Core functions */
char *man_get_sections_json(void);
char *man_get_section_pages_json(const char *area, const char *section);
char *man_render_page(const char *area, const char *section, const char *name,
		      const char *format);
int verify_man_page(const char *man_path, const char *section,
		    const char *name);
char *man_get_page_metadata_json(const char *area, const char *section,
				 const char *name);
char *man_search_json(const char *query);
const char *determine_man_area(const char *name, const char *section);

/* HTTP Handlers */
int man_render_handler(void *cls, struct MHD_Connection *connection,
		       const char *url, const char *method, const char *version,
		       const char *upload_data, size_t *upload_data_size,
		       void **con_cls);

int man_api_handler(void *cls, struct MHD_Connection *connection,
		    const char *url, const char *method, const char *version,
		    const char *upload_data, size_t *upload_data_size,
		    void **con_cls);

#endif
