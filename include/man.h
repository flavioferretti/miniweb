/* man.h - Manual pages handler (Native kqueue version) */
#ifndef MAN_H
#define MAN_H

#include "http_handler.h"
#include <stddef.h>

/* --- Core API Functions --- */
char *man_get_sections_json(void);
char *man_get_section_pages_json(const char *area, const char *section);
char *man_get_page_metadata_json(const char *area, const char *section, const char *name);
char *man_api_search(const char *query);

/**
 * Renders a man page using mandoc.
 * @return Dynamically allocated string with the output.
 */
// char *man_render_page(const char *area, const char *section, const char *page, const char *format);

/* --- HTTP Handlers (kqueue compatible) --- */

/**
 * Handles the visual rendering of man pages (HTML, etc.)
 */
int man_render_handler(http_request_t *req);

/**
 * Handles JSON API requests for man pages.
 */
int man_api_handler(http_request_t *req);

char *man_render_page(const char *area, const char *section, const char *page, const char *format, size_t *out_len);

#endif /* MAN_H */
