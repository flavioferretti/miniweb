#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#include <microhttpd.h>
#include <stdarg.h>
#include <stddef.h>

/* Create error responses */
struct MHD_Response *http_error_response(unsigned int status_code,
					 const char *format, ...)
    __attribute__((format(printf, 2, 3)));
struct MHD_Response *http_error_text(unsigned int status_code,
				     const char *message);
struct MHD_Response *http_error_json(unsigned int status_code,
				     const char *error_msg, int errno_val);

/* Queue error responses directly */
int http_queue_error(struct MHD_Connection *connection,
		     unsigned int status_code, const char *message);
int http_queue_400(struct MHD_Connection *connection, const char *message);
int http_queue_403(struct MHD_Connection *connection, const char *message);
int http_queue_404(struct MHD_Connection *connection, const char *path);
int http_queue_500(struct MHD_Connection *connection, const char *details);

/* JSON helper */
char *json_escape_string(const char *src);

/* Command execution helper with cleanup */
char *safe_popen_read(const char *cmd, size_t max_size);

/* Safe string copy with sanitization */
void sanitize_string(char *s);

#endif
