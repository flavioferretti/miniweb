#ifndef MINIWEB_MODULES_MAN_INTERNAL_H
#define MINIWEB_MODULES_MAN_INTERNAL_H

#include <stddef.h>
#include <miniweb/http/handler.h>

#define MAN_MAX_JSON_SIZE       (256 * 1024)
#define MAN_MAX_OUTPUT_SIZE     (10 * 1024 * 1024)
#define MAN_FS_CACHE_TTL_SEC    300

int man_path_matches_endpoint(const char *path, const char *endpoint);
int man_get_query_value(const char *url, const char *key, char *out, size_t out_size);
int man_parse_section_from_filename(const char *filename, char *section_out,
                                    size_t section_out_len);
int man_is_valid_token(const char *s);
int man_is_valid_section(const char *section);
const char *man_area_from_path(const char *filepath);

char *man_resolve_path(const char *name, const char *section);
char *man_api_search_raw(const char *query);

const char *man_mime_for_format(const char *format);
void man_add_content_disposition_for_format(http_response_t *resp,
                                            const char *format,
                                            const char *page);
void man_strip_overstrike_ascii(char *text, size_t *len);
void man_render_cache_cleanup(void);

#endif
