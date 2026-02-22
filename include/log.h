#ifndef LOG_H
#define LOG_H

#include <stdarg.h>

int log_init(const char *path, int verbose);
void log_close(void);
void log_set_verbose(int verbose);
void log_info(const char *fmt, ...);
void log_debug(const char *fmt, ...);
void log_error(const char *fmt, ...);
void log_errno(const char *context);

#endif
