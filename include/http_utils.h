/* http_utils.h - HTTP utility functions (no MHD dependency) */

#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#include <stddef.h>

/** Escape a UTF-8 string for safe embedding in JSON values. */
char *json_escape_string(const char *src);

/** Replace characters unsafe for filesystem usage with '_'. */
void sanitize_string(char *s);

/**
 * Execute a shell command and capture stdout/stderr up to max_size bytes.
 *
 * @param cmd Command string executed by the shell.
 * @param max_size Maximum output size accepted before truncation/failure.
 * @return malloc-allocated NUL-terminated output buffer, or NULL on error.
 */
char *safe_popen_read(const char *cmd, size_t max_size);

/**
 * Execute a binary with argv[] and capture stdout/stderr with timeout control.
 *
 * @param path Executable absolute/relative path.
 * @param argv Argument vector for execv (must be NULL-terminated).
 * @param max_size Maximum output bytes allowed.
 * @param timeout_seconds Hard timeout for child process.
 * @param out_len Optional output length in bytes.
 * @return malloc-allocated output buffer, or NULL on failure/timeout.
 */
char *safe_popen_read_argv(const char *path, char *const argv[],
							   size_t max_size, int timeout_seconds, size_t *out_len);

#endif /* HTTP_UTILS_H */
