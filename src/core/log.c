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
 * @brief TODO: Describe log_timestamp.
 * @param buf TODO: Describe this parameter.
 * @param len TODO: Describe this parameter.
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
 * @brief TODO: Describe log_init.
 * @param path TODO: Describe this parameter.
 * @param verbose TODO: Describe this parameter.
 * @return TODO: Describe the return value.
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
 * @brief TODO: Describe log_close.
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
 * @brief TODO: Describe log_set_verbose.
 * @param verbose TODO: Describe this parameter.
 */
void
log_set_verbose(int verbose)
{
    log_verbose = verbose;
}

/**
 * @brief TODO: Describe log_write.
 * @param level TODO: Describe this parameter.
 * @param fmt TODO: Describe this parameter.
 * @param ap TODO: Describe this parameter.
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
 * @brief TODO: Describe log_info.
 * @param fmt TODO: Describe this parameter.
 * @param param TODO: Describe this parameter.
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
 * @brief TODO: Describe log_debug.
 * @param fmt TODO: Describe this parameter.
 * @param param TODO: Describe this parameter.
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
 * @brief TODO: Describe log_error.
 * @param fmt TODO: Describe this parameter.
 * @param param TODO: Describe this parameter.
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
 * @brief TODO: Describe log_errno.
 * @param context TODO: Describe this parameter.
 */
void
log_errno(const char *context)
{
    log_error("%s: %s", context, strerror(errno));
}
