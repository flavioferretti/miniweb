#ifndef MINIWEB_HTTP_RESPONSE_INTERNAL_H
#define MINIWEB_HTTP_RESPONSE_INTERNAL_H

#include <miniweb/http/handler.h>
#include <sys/stat.h>
#include <sys/uio.h>

#define WRITE_RETRY_LIMIT 5
#define WRITE_WAIT_MS 50
#define FILE_CACHE_SLOTS 32
#define FILE_CACHE_MAX_BYTES (256 * 1024)
#define FILE_CACHE_INSERTS_PER_SEC 8
#define FILE_CACHE_MAX_AGE_SEC 240
#define RESPONSE_POOL_SHARDS 16
#define FILE_CACHE_SHARDS 16

void http_handler_globals_init_once(void);
void http_response_pool_init_shards(void);

http_response_t *http_response_pool_acquire(void);
int http_response_pool_release(http_response_t *resp);

int http_file_cache_lookup(const char *path, const struct stat *st, char **out,
    size_t *out_len);
void http_file_cache_store(const char *path, const struct stat *st,
    const char *data, size_t len);

int http_response_write_all(int fd, const void *buf, size_t n);
int http_response_writev_all(int fd, struct iovec *iov, int iovcnt);
const char *http_response_status_text(int status_code);

#endif
