#include "../include/log.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *g_log_file;
static int g_verbose;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void
log_vwrite(const char *level, int debug_only, const char *fmt, va_list ap)
{
	if (debug_only && !g_verbose)
		return;

	FILE *out = g_log_file ? g_log_file : stderr;
	time_t now = time(NULL);
	struct tm tm_now;
	localtime_r(&now, &tm_now);

	char ts[32];
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);

	pthread_mutex_lock(&g_log_lock);
	fprintf(out, "%s [%s] ", ts, level);
	vfprintf(out, fmt, ap);
	fputc('\n', out);
	fflush(out);
	pthread_mutex_unlock(&g_log_lock);
}

int
log_init(const char *path, int verbose)
{
	g_verbose = verbose;
	if (!path || path[0] == '\0')
		return 0;

	FILE *f = fopen(path, "a");
	if (!f)
		return -1;

	setvbuf(f, NULL, _IOLBF, 0);
	g_log_file = f;
	return 0;
}

void
log_set_verbose(int verbose)
{
	g_verbose = verbose;
}

void
log_close(void)
{
	pthread_mutex_lock(&g_log_lock);
	if (g_log_file) {
		fclose(g_log_file);
		g_log_file = NULL;
	}
	pthread_mutex_unlock(&g_log_lock);
}

void
log_info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	log_vwrite("INFO", 0, fmt, ap);
	va_end(ap);
}

void
log_debug(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	log_vwrite("DEBUG", 1, fmt, ap);
	va_end(ap);
}

void
log_error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	log_vwrite("ERROR", 0, fmt, ap);
	va_end(ap);
}

void
log_errno(const char *context)
{
	log_error("%s: %s", context, strerror(errno));
}
