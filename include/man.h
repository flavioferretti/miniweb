/* man.h - Manual pages handler (Native kqueue version) */
#ifndef MAN_H
#define MAN_H

#include "http_handler.h"
#include <stddef.h>

/* --- Core API Functions --- */

/** Return all available manual sections as JSON. */
char *man_get_sections_json(void);

/** Return pages for a specific area/section as JSON. */
char *man_get_section_pages_json(const char *area, const char *section);

/** Return metadata for one manual page as JSON. */
char *man_get_page_metadata_json(const char *area, const char *section, const char *name);

/** Search manual pages and return JSON search results. */
char *man_api_search(const char *query);

/**
 * Render a man page through mandoc.
 *
 * @param area Manual area (for example "usr.bin").
 * @param section Manual section (for example "1").
 * @param page Manual page name.
 * @param format Output format (html, utf8, markdown, pdf).
 * @param out_len Output byte length of the rendered buffer.
 * @return Dynamically allocated rendered output, or NULL on failure.
 */
char *man_render_page(const char *area, const char *section, const char *page, const char *format, size_t *out_len);

/* --- HTTP Handlers (kqueue compatible) --- */

/** Handle visual man page rendering requests (HTML and related formats). */
int man_render_handler(http_request_t *req);

/** Handle JSON API requests for manual page discovery/search. */
int man_api_handler(http_request_t *req);

#endif /* MAN_H */
