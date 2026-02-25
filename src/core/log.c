/* log.c - Logging implementation for miniweb */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#include <miniweb/core/log.h>

static FILE            *log_fp      = NULL;
static int              log_verbose = 0;
static pthread_mutex_t  log_mutex   = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Write an ISO-8601 timestamp into @p buf.
 * @param buf Destination buffer.
 * @param len Capacity of @p buf in bytes.
 */
static void
log_timestamp(char *buf, size_t len)
{
    time_t     now = time(NULL);
    struct tm  tm;

    localtime_r(&now, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}

/**
 * @brief Open the log destination and configure verbosity.
 *
 * When @p path is NULL or empty, log output goes to stderr.
 *
 * @param path    Log file path, or NULL/empty for stderr.
 * @param verbose Non-zero to enable debug-level messages.
 * @return 0 on success, -1 when the log file cannot be opened.
 */
int
log_init(const char *path, int verbose)
{
    log_verbose = verbose;

    if (path != NULL && path[0] != '\0') {
        log_fp = fopen(path, "a");
        if (log_fp == NULL) {
            fprintf(stderr, "log_init: cannot open log file '%s': %s\n",
                    path, strerror(errno));
            return -1;
        }
    } else {
        log_fp = stderr;
    }

    return 0;
}

/**
 * @brief Flush and close the log file opened by log_init().
 */
void
log_close(void)
{
    pthread_mutex_lock(&log_mutex);
    if (log_fp != NULL && log_fp != stderr) {
        fclose(log_fp);
    }
    log_fp = NULL;
    pthread_mutex_unlock(&log_mutex);
}

/**
 * @brief Enable or disable debug-level log output at runtime.
 * @param verbose Non-zero to enable debug messages.
 */
void
log_set_verbose(int verbose)
{
    log_verbose = verbose;
}

/**
 * @brief Format and emit one log line with the given severity label.
 * @param level Severity label string (e.g., "INFO", "ERROR").
 * @param fmt   printf-style format string.
 * @param ap    Variadic argument list.
 */
static void
log_write(const char *level, const char *fmt, va_list ap)
{
    char ts[32];
    FILE *fp;

    log_timestamp(ts, sizeof(ts));

    pthread_mutex_lock(&log_mutex);
    fp = (log_fp != NULL) ? log_fp : stderr;
    fprintf(fp, "[%s] [%s] ", ts, level);
    vfprintf(fp, fmt, ap);
    fputc('\n', fp);
    fflush(fp);
    pthread_mutex_unlock(&log_mutex);
}

/**
 * @brief Emit an INFO-level log message.
 * @param fmt printf-style format string followed by variadic arguments.
 */
void
log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_write("INFO", fmt, ap);
    va_end(ap);
}

/**
 * @brief Emit a DEBUG-level log message (only when verbose mode is active).
 * @param fmt printf-style format string followed by variadic arguments.
 */
void
log_debug(const char *fmt, ...)
{
    if (!log_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    log_write("DEBUG", fmt, ap);
    va_end(ap);
}

/**
 * @brief Emit an ERROR-level log message.
 * @param fmt printf-style format string followed by variadic arguments.
 */
void
log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_write("ERROR", fmt, ap);
    va_end(ap);
}

/**
 * @brief Emit an ERROR-level log message that includes strerror(errno).
 * @param context Short description of the operation that failed.
 */
void
log_errno(const char *context)
{
    log_error("%s: %s", context, strerror(errno));
}
