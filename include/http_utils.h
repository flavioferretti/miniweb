/* http_utils.h - HTTP utility functions (no MHD dependency) */

#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#include <stddef.h>

/* JSON string escaping — caller must free() the result */
char *json_escape_string(const char *src);

/* Replace characters unsafe for filesystem use with '_' */
void sanitize_string(char *s);

/* Execute a command via fork+execv, capture stdout+stderr.
 * Returns a malloc'd NUL-terminated string, or NULL on error/timeout.
 * Caller must free(). */
char *safe_popen_read(const char *cmd, size_t max_size);
char *safe_popen_read_argv(const char *path, char *const argv[],
						   size_t max_size, int timeout_seconds, size_t *out_len);

#endif /* HTTP_UTILS_H */
