/* man_module.c - man page rendering module
 *
 * Two-level caching strategy:
 *   L1: In-memory sharded render cache (RAM)
 *   L2: Filesystem cache under static/man/
 *   Fallback: mandoc subprocess for cache misses on both levels
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <miniweb/router/router.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <miniweb/core/config.h>
#include <miniweb/core/log.h>
#include <miniweb/http/handler.h>
#include <miniweb/http/utils.h>
#include <miniweb/modules/man.h>
#include <miniweb/router/routes.h>

/* =========================================================================
 * Tunables
 * ========================================================================= */

#define MAX_JSON_SIZE       (256 * 1024)
#define MAX_OUTPUT_SIZE     (10 * 1024 * 1024)  /* 10 MiB hard ceiling */
#define MAN_FS_CACHE_TTL_SEC    300             /* filesystem cache TTL */

/* In-process render cache.
 * 8 shards × 64 slots = 512 cached pages.  Each slot holds a heap copy of
 * the rendered body so requests are served without any open() call.
 * TTL of 600 s keeps memory bounded while surviving benchmark runs.
 */
#define MAN_RENDER_CACHE_SHARDS 8
#define MAN_RENDER_CACHE_SLOTS  64
#define MAN_RENDER_CACHE_TTL    600

/* =========================================================================
 * In-process render cache types
 * ========================================================================= */

typedef struct {
    char   key[192];    /* "area\0section\0page\0format" packed as one string */
    char  *body;        /* malloc'd rendered content */
    size_t len;
    time_t inserted;
} man_render_slot_t;

typedef struct {
    man_render_slot_t slots[MAN_RENDER_CACHE_SLOTS];
    pthread_mutex_t   lock;
    unsigned int      hits;
    unsigned int      misses;
    unsigned int      evictions;
} man_render_shard_t;

static man_render_shard_t g_man_cache[MAN_RENDER_CACHE_SHARDS];
static pthread_once_t     g_man_cache_once = PTHREAD_ONCE_INIT;


/* =========================================================================
 * Forward declarations
 * ========================================================================= */

static int         is_valid_section(const char *section);
static char       *select_resolved_man_path(char *raw_output);
static int         compare_string_ptrs(const void *a, const void *b);

/* =========================================================================
 * In-process cache helpers
 * ========================================================================= */

static void
man_render_cache_init(void)
{
    for (int i = 0; i < MAN_RENDER_CACHE_SHARDS; i++)
        pthread_mutex_init(&g_man_cache[i].lock, NULL);
}

/* Build a compact lookup key: "area/section/page.format" */
static void
man_render_cache_key(const char *area, const char *section,
                     const char *page, const char *format,
                     char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s/%s.%s", area, section, page, format);
}

/* FNV-1a shard selection */
static int
man_render_cache_shard(const char *key)
{
    unsigned long h = 14695981039346656037UL;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        h ^= *p;
        h *= 1099511628211UL;
    }
    return (int)(h % MAN_RENDER_CACHE_SHARDS);
}

/**
 * @brief Look up a rendered page body in the in-process cache.
 * @param area Manual area (system, packages, x11)
 * @param section Manual section (1, 2, 3, etc.)
 * @param page Manual page name
 * @param format Output format (html, pdf, txt, etc.)
 * @param out_len Receives the length of the returned body
 * @return Malloc'd copy of the body on hit, NULL on miss. Caller must free.
 */
static char *
man_render_cache_get(const char *area, const char *section,
                     const char *page, const char *format,
                     size_t *out_len)
{
    char key[192];
    man_render_cache_key(area, section, page, format, key, sizeof(key));
    int si = man_render_cache_shard(key);
    man_render_shard_t *shard = &g_man_cache[si];

    pthread_once(&g_man_cache_once, man_render_cache_init);
    pthread_mutex_lock(&shard->lock);

    time_t now = time(NULL);
    for (int i = 0; i < MAN_RENDER_CACHE_SLOTS; i++) {
        man_render_slot_t *s = &shard->slots[i];
        if (s->body == NULL)
            continue;
        if (strcmp(s->key, key) != 0)
            continue;
        /* TTL check */
        if ((now - s->inserted) > MAN_RENDER_CACHE_TTL) {
            free(s->body);
            s->body = NULL;
            s->len  = 0;
            shard->evictions++;
            break;
        }
        /* Hit: return a copy so the caller owns the buffer */
        char *copy = malloc(s->len + 1);
        if (copy) {
            memcpy(copy, s->body, s->len);
            copy[s->len] = '\0';
            *out_len = s->len;
            shard->hits++;
            pthread_mutex_unlock(&shard->lock);
            return copy;
        }
        break;
    }
    shard->misses++;
    pthread_mutex_unlock(&shard->lock);
    return NULL;
}

/**
 * @brief Insert a rendered page body into the in-process cache.
 * @param area Manual area
 * @param section Manual section
 * @param page Manual page name
 * @param format Output format
 * @param body Rendered content (caller still owns it)
 * @param len Length of body
 */
static void
man_render_cache_put(const char *area, const char *section,
                     const char *page, const char *format,
                     const char *body, size_t len)
{
    if (!body || len == 0)
        return;

    char key[192];
    man_render_cache_key(area, section, page, format, key, sizeof(key));
    int si = man_render_cache_shard(key);
    man_render_shard_t *shard = &g_man_cache[si];

    pthread_once(&g_man_cache_once, man_render_cache_init);
    pthread_mutex_lock(&shard->lock);

    time_t now = time(NULL);
    int    target = -1;
    time_t oldest = now + 1;

    for (int i = 0; i < MAN_RENDER_CACHE_SLOTS; i++) {
        man_render_slot_t *s = &shard->slots[i];

        /* Update in place if key already present */
        if (s->body && strcmp(s->key, key) == 0) {
            free(s->body);
            s->body     = malloc(len + 1);
            if (s->body) {
                memcpy(s->body, body, len);
                s->body[len] = '\0';
                s->len      = len;
                s->inserted = now;
            } else {
                s->body = NULL;
                s->len  = 0;
            }
            pthread_mutex_unlock(&shard->lock);
            return;
        }

        /* Track empty or oldest slot for eviction */
        if (s->body == NULL) {
            target = i;
            break;
        }
        if (s->inserted < oldest) {
            oldest = s->inserted;
            target = i;
        }
    }

    if (target < 0) {
        pthread_mutex_unlock(&shard->lock);
        return;
    }

    man_render_slot_t *s = &shard->slots[target];
    free(s->body);
    s->body = malloc(len + 1);
    if (s->body) {
        memcpy(s->body, body, len);
        s->body[len] = '\0';
        strlcpy(s->key, key, sizeof(s->key));
        s->len      = len;
        s->inserted = now;
    } else {
        s->body = NULL;
        s->len  = 0;
    }

    pthread_mutex_unlock(&shard->lock);
}

/* =========================================================================
 * String / path helpers
 * ========================================================================= */

static int
compare_string_ptrs(const void *a, const void *b)
{
    const char *const *left  = a;
    const char *const *right = b;
    return strcmp(*left, *right);
}

static int
path_matches_endpoint(const char *path, const char *endpoint)
{
    size_t len;
    if (!path || !endpoint)
        return 0;
    len = strlen(endpoint);
    if (strncmp(path, endpoint, len) != 0)
        return 0;
    return path[len] == '\0' || path[len] == '?';
}

/**
 * @brief Decode URL-encoded text into a destination buffer.
 */
static int
url_decode_into(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;
    if (!src || !dst || dst_size == 0)
        return -1;
    for (size_t si = 0; src[si] != '\0'; si++) {
        if (di + 1 >= dst_size)
            return -1;
        if (src[si] == '+') {
            dst[di++] = ' ';
            continue;
        }
        if (src[si] == '%' && isxdigit((unsigned char)src[si + 1]) &&
            isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = {src[si + 1], src[si + 2], '\0'};
        dst[di++] = (char)strtol(hex, NULL, 16);
        si += 2;
        continue;
            }
            dst[di++] = src[si];
    }
    dst[di] = '\0';
    return 0;
}

static int
get_query_value(const char *url, const char *key, char *out, size_t out_size)
{
    const char *qs;
    size_t key_len;

    if (!url || !key || !out || out_size == 0)
        return 0;
    qs = strchr(url, '?');
    if (!qs)
        return 0;
    qs++;
    key_len = strlen(key);

    while (*qs) {
        const char *entry = qs;
        const char *eq    = strchr(entry, '=');
        const char *amp   = strchr(entry, '&');
        if (!amp)
            amp = entry + strlen(entry);
        if (!eq || eq > amp) {
            qs = (*amp == '&') ? amp + 1 : amp;
            continue;
        }
        if ((size_t)(eq - entry) == key_len &&
            strncmp(entry, key, key_len) == 0) {
            char encoded[512];
            size_t encoded_len = (size_t)(amp - (eq + 1));
            if (encoded_len >= sizeof(encoded))
                return 0;
            memcpy(encoded, eq + 1, encoded_len);
            encoded[encoded_len] = '\0';
            return url_decode_into(encoded, out, out_size) == 0;
        }
        qs = (*amp == '&') ? amp + 1 : amp;
    }
    return 0;
}

static int
parse_section_from_filename(const char *filename, char *section_out,
                            size_t section_out_len)
{
    static const char *compressed_suffixes[] = {".gz", ".bz2", ".xz", ".zst"};
    char tmp[256];

    if (!filename || !section_out || section_out_len == 0)
        return 0;
    strlcpy(tmp, filename, sizeof(tmp));

    for (size_t i = 0;
         i < sizeof(compressed_suffixes) / sizeof(compressed_suffixes[0]);
    i++) {
        size_t tlen = strlen(tmp);
        size_t slen = strlen(compressed_suffixes[i]);
        if (tlen > slen &&
            strcmp(tmp + tlen - slen, compressed_suffixes[i]) == 0) {
            tmp[tlen - slen] = '\0';
        break;
            }
    }

    char *dot = strrchr(tmp, '.');
    if (!dot || dot[1] == '\0' || !is_valid_section(dot + 1))
        return 0;
    strlcpy(section_out, dot + 1, section_out_len);
    return 1;
}

static void
strip_overstrike_ascii(char *text, size_t *len)
{
    if (!text || !len)
        return;
    size_t in = 0, out = 0;
    while (in < *len && text[in] != '\0') {
        if (in + 2 < *len && text[in + 1] == '\b') {
            text[out++] = text[in + 2];
            in += 3;
            continue;
        }
        text[out++] = text[in++];
    }
    text[out] = '\0';
    *len = out;
}

static int
is_valid_token(const char *s)
{
    if (!s || *s == '\0')
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-' && *p != '+')
            return 0;
    }
    return 1;
}

static int
is_valid_section(const char *section)
{
    if (!section || *section == '\0' || strlen(section) > 8)
        return 0;
    for (const unsigned char *p = (const unsigned char *)section; *p; p++) {
        if (!isalnum(*p))
            return 0;
    }
    return 1;
}

/* =========================================================================
 * Man path resolution
 * ========================================================================= */

static char *
select_resolved_man_path(char *raw_output)
{
    char *selected = NULL;
    if (!raw_output)
        return NULL;

    /* Normalise whitespace */
    for (char *p = raw_output; *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
    }

    char *saveptr = NULL;
    for (char *tok = strtok_r(raw_output, " ", &saveptr); tok;
         tok = strtok_r(NULL, " ", &saveptr)) {
        if (tok[0] != '/')
            continue;
        if (!selected)
            selected = tok;
        if (access(tok, R_OK) == 0) {
            selected = tok;
            break;
        }
         }

         if (!selected || selected[0] == '\0') {
             free(raw_output);
             return NULL;
         }

         char *resolved = strdup(selected);
         free(raw_output);
         return resolved;
}

static char *
resolve_man_path(const char *name, const char *section)
{
    if (!is_valid_token(name) || !is_valid_section(section))
        return NULL;

    char *const argv[] = {
        "man",
        "-M", "/usr/share/man:/usr/local/man:/usr/X11R6/man",
        "-w",
        (char *)section,
        (char *)name,
        NULL
    };
    char *raw = safe_popen_read_argv("/usr/bin/man", argv, 2048, 5, NULL);
    return select_resolved_man_path(raw);
}

/* =========================================================================
 * Filesystem cache helpers
 * ========================================================================= */

static int
build_cache_paths(const char *area, const char *section, const char *page,
                  const char *format,
                  char *rel, size_t rel_len,
                  char *abs_path, size_t abs_len)
{
    if (!area || !section || !page || !format)
        return -1;

    if (rel && rel_len > 0) {
        int n = snprintf(rel, rel_len, "/static/man/%s/%s/%s.%s",
                         area, section, page, format);
        if (n < 0 || (size_t)n >= rel_len)
            return -1;
    }

    if (abs_path && abs_len > 0) {
        int n = snprintf(abs_path, abs_len, "%s/man/%s/%s/%s.%s",
                         config_static_dir, area, section, page, format);
        if (n < 0 || (size_t)n >= abs_len)
            return -1;
    }
    return 0;
}

static int
mkdir_p(const char *dir, const char *base_dir)
{
    if (!dir || *dir == '\0')
        return -1;
    char tmp[512];
    strlcpy(tmp, dir, sizeof(tmp));
    char *start = tmp;
    if (base_dir && *base_dir) {
        size_t base_len = strlen(base_dir);
        if (strncmp(tmp, base_dir, base_len) == 0) {
            start = tmp + base_len;
            if (*start == '/')
                start++;
        }
    }
    for (char *p = start; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int
write_file_binary(const char *path, const char *buf, size_t len)
{
    if (!path || !buf || len == 0)
        return -1;
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w <= 0) {
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }
    close(fd);
    return 0;
}

static const char *
mime_for_format(const char *format)
{
    if (strcmp(format, "pdf") == 0) return "application/pdf";
    if (strcmp(format, "ps")  == 0) return "application/postscript";
    if (strcmp(format, "md")  == 0) return "text/markdown; charset=utf-8";
    if (strcmp(format, "txt") == 0) return "text/plain; charset=utf-8";
    return "text/html; charset=utf-8";
}

static void
add_content_disposition_for_format(http_response_t *resp, const char *format,
                                   const char *page)
{
    char content_disp[256];
    if (strcmp(format, "pdf") == 0) {
        snprintf(content_disp, sizeof(content_disp),
                 "inline; filename=\"%s.pdf\"", page);
        http_response_add_header(resp, "Content-Disposition", content_disp);
    } else if (strcmp(format, "md") == 0 || strcmp(format, "txt") == 0) {
        http_response_add_header(resp, "Content-Disposition", "inline");
    } else if (strcmp(format, "ps") == 0) {
        snprintf(content_disp, sizeof(content_disp),
                 "attachment; filename=\"%s.ps\"", page);
        http_response_add_header(resp, "Content-Disposition", content_disp);
    }
}

static const char *
area_from_path(const char *filepath)
{
    if (strncmp(filepath, "/usr/X11R6/", 11) == 0) return "x11";
    if (strncmp(filepath, "/usr/local/",  11) == 0) return "packages";
    return "system";
}

/* =========================================================================
 * man_render_page
 * ========================================================================= */

char *
man_render_page(const char *area, const char *section, const char *page,
                const char *format, size_t *out_len)
{
    (void)area; /* MANPATH covers all trees */

    /* 1. Resolve physical path — single subprocess. */
    char *filepath = NULL;
    if (is_valid_section(section))
        filepath = resolve_man_path(page, section);

    if (!filepath || filepath[0] != '/') {
        free(filepath);
        return NULL;
    }

    /* 2. Select mandoc output format. */
    const char *t_arg = "html";
    if      (strcmp(format, "pdf") == 0) t_arg = "pdf";
    else if (strcmp(format, "ps")  == 0) t_arg = "ps";
    else if (strcmp(format, "md")  == 0) t_arg = "markdown";
    else if (strcmp(format, "txt") == 0) t_arg = "ascii";

    /* 3. Execute mandoc. */
    char *argv_m[10];
    int argc = 0;
    argv_m[argc++] = "mandoc";
    argv_m[argc++] = "-T";
    argv_m[argc++] = (char *)t_arg;
    if (strcmp(t_arg, "html") == 0)
        argv_m[argc++] = "-Ostyle=/static/css/custom.css";
    argv_m[argc++] = filepath;
    argv_m[argc++] = NULL;

    char *output = safe_popen_read_argv("/usr/bin/mandoc", argv_m,
                                        MAX_OUTPUT_SIZE, 10, out_len);

    /* Normalise ASCII output */
    if (output && *out_len > 0 && strcmp(format, "txt") == 0)
        strip_overstrike_ascii(output, out_len);

    /* Fallback: markdown → ascii when mandoc cannot produce markdown */
    if (!output && strcmp(format, "md") == 0) {
        char *const argv_ascii[] = {"mandoc", "-T", "ascii", filepath, NULL};
        output = safe_popen_read_argv("/usr/bin/mandoc", argv_ascii,
                                      MAX_OUTPUT_SIZE, 10, out_len);
        if (output && *out_len > 0)
            strip_overstrike_ascii(output, out_len);
    }

    free(filepath);
    return output;
}

/* =========================================================================
 * man_render_handler - Two-level caching implementation
 * ========================================================================= */

int
man_render_handler(http_request_t *req)
{
    char area[32]    = "system";
    char section[16] = "";
    char page[64]    = "";
    char format[16]  = "html";

    /* --- 1. Parse URL: /man/{area}/{section}/{page}[.format] --- */
    if (strncmp(req->url, "/man/", 5) != 0)
        return http_send_error(req, 400, "Invalid URL");

    const char *p = req->url + 5;
    char path_copy[256];
    strlcpy(path_copy, p, sizeof(path_copy));

    /* Strip query string */
    char *qs = strchr(path_copy, '?');
    if (qs) *qs = '\0';

    /* Thread-safe parsing with strtok_r */
    char *saveptr = NULL;
    char *token   = strtok_r(path_copy, "/", &saveptr);
    if (token) strlcpy(area, token, sizeof(area));
    token = strtok_r(NULL, "/", &saveptr);
    if (token) strlcpy(section, token, sizeof(section));
    token = strtok_r(NULL, "/", &saveptr);
    if (token) {
        char *dot = strrchr(token, '.');
        if (dot) {
            *dot = '\0';
            strlcpy(format, dot + 1, sizeof(format));
        }
        strlcpy(page, token, sizeof(page));
    }

    /* --- 2. Minimal validation --- */
    if (page[0] == '\0' || section[0] == '\0')
        return http_send_error(req, 400, "Missing section or page name");

    if (strcmp(format, "html") != 0 && strcmp(format, "pdf") != 0 &&
        strcmp(format, "ps")   != 0 && strcmp(format, "md")  != 0 &&
        strcmp(format, "txt")  != 0)
        return http_send_error(req, 400, "Unsupported format");

    /* --- 3. Initialize cache on first use --- */
    pthread_once(&g_man_cache_once, man_render_cache_init);

    char *response_body = NULL;
    size_t response_len = 0;

    /* --- 4. Attempt to serve from L1 cache (RAM) --- */
    response_body = man_render_cache_get(area, section, page, format, &response_len);
    if (response_body) {
        log_debug("[MAN] L1 cache HIT %s/%s/%s.%s", area, section, page, format);
        goto send_response;
    }
    log_debug("[MAN] L1 cache MISS %s/%s/%s.%s", area, section, page, format);

    /* --- 5. Attempt to serve from L2 cache (Filesystem) --- */
    char cache_abs[512];
    int have_paths = (build_cache_paths(area, section, page, format,
                                        NULL, 0, cache_abs, sizeof(cache_abs)) == 0);

    if (have_paths && access(cache_abs, R_OK) == 0) {
        log_debug("[MAN] L2 cache HIT %s", cache_abs);
        int fd = open(cache_abs, O_RDONLY);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size > 0) {
                response_body = malloc((size_t)st.st_size + 1);
                if (response_body) {
                    ssize_t nr = read(fd, response_body, (size_t)st.st_size);
                    if (nr == (ssize_t)st.st_size) {
                        response_body[nr] = '\0';
                        response_len = (size_t)nr;
                        close(fd);
                        /* Promote to L1 cache */
                        man_render_cache_put(area, section, page, format,
                                             response_body, response_len);
                        goto send_response;
                    }
                    free(response_body);
                    response_body = NULL;
                }
            }
            close(fd);
        }
        /* If read fails, fall through to mandoc */
    }
    log_debug("[MAN] L2 cache MISS %s/%s/%s.%s", area, section, page, format);

    /* --- 6. Cache miss on both levels: render via mandoc --- */
    log_debug("[MAN] Rendering via mandoc %s/%s/%s.%s", area, section, page, format);
    response_body = man_render_page(area, section, page, format, &response_len);
    if (!response_body) {
        return http_send_error(req, 404, "Manual page not found");
    }

    /* --- 7. Store the newly rendered page in both caches --- */
    man_render_cache_put(area, section, page, format, response_body, response_len);

    if (have_paths) {
        char cache_dir[512];
        strlcpy(cache_dir, cache_abs, sizeof(cache_dir));
        char *last_slash = strrchr(cache_dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            if (mkdir_p(cache_dir, config_static_dir) == 0)
                (void)write_file_binary(cache_abs, response_body, response_len);
        }
    }

    /* --- 8. Send the final response --- */
    send_response: {
        http_response_t *resp = http_response_create();
        if (!resp) {
            free(response_body);
            return -1;
        }

        resp->content_type = mime_for_format(format);
        http_response_add_header(resp, "Cache-Control", "public, max-age=300");
        add_content_disposition_for_format(resp, format, page);
        http_response_set_body(resp, response_body, response_len, 1); /* 1 = free_body */

        int ret = http_response_send(req, resp);
        http_response_free(resp);
        return ret;
    }
}

/* =========================================================================
 * API JSON helpers
 * ========================================================================= */

char *
man_get_sections_json(void)
{
    return strdup(
        "{\"system\":{"
        "\"name\":\"OpenBSD Base System\","
        "\"path\":\"/usr/share/man\","
        "\"sections\":["
        "{\"id\":\"1\",\"name\":\"General Commands\"},"
        "{\"id\":\"2\",\"name\":\"System Calls\"},"
        "{\"id\":\"3\",\"name\":\"Library Functions\"},"
        "{\"id\":\"3p\",\"name\":\"Perl Library\"},"
        "{\"id\":\"4\",\"name\":\"Device Drivers\"},"
        "{\"id\":\"5\",\"name\":\"File Formats\"},"
        "{\"id\":\"6\",\"name\":\"Games\"},"
        "{\"id\":\"7\",\"name\":\"Miscellaneous\"},"
        "{\"id\":\"8\",\"name\":\"System Administration\"},"
        "{\"id\":\"9\",\"name\":\"Kernel Internals\"}"
        "]},"
        "\"x11\":{"
        "\"name\":\"X11 Window System\","
        "\"path\":\"/usr/X11R6/man\","
        "\"sections\":["
        "{\"id\":\"1\",\"name\":\"X11 Commands\"},"
        "{\"id\":\"3\",\"name\":\"X11 Library\"},"
        "{\"id\":\"4\",\"name\":\"X11 Drivers\"},"
        "{\"id\":\"5\",\"name\":\"X11 Formats\"},"
        "{\"id\":\"7\",\"name\":\"X11 Misc\"}"
        "]},"
        "\"packages\":{"
        "\"name\":\"Local Packages\","
        "\"path\":\"/usr/local/man\","
        "\"sections\":["
        "{\"id\":\"1\",\"name\":\"Pkg General\"},"
        "{\"id\":\"2\",\"name\":\"Pkg Calls\"},"
        "{\"id\":\"3\",\"name\":\"Pkg Lib\"},"
        "{\"id\":\"3p\",\"name\":\"Pkg Perl\"},"
        "{\"id\":\"4\",\"name\":\"Pkg Drivers\"},"
        "{\"id\":\"5\",\"name\":\"Pkg Formats\"},"
        "{\"id\":\"6\",\"name\":\"Pkg Games\"},"
        "{\"id\":\"7\",\"name\":\"Pkg Misc\"},"
        "{\"id\":\"8\",\"name\":\"Pkg Admin\"},"
        "{\"id\":\"9\",\"name\":\"Pkg Kernel\"}"
        "]}}");
}

char *
man_get_section_pages_json(const char *area, const char *section)
{
    char dir_path[256];
    char *pages[8192];
    size_t page_count = 0;
    const char *base = "/usr/share/man";

    if (strcmp(area, "packages") == 0)
        base = "/usr/local/man";
    else if (strcmp(area, "x11") == 0)
        base = "/usr/X11R6/man";

    snprintf(dir_path, sizeof(dir_path), "%s/man%s", base, section);

    DIR *dr = opendir(dir_path);
    if (!dr)
        return strdup("{\"pages\":[]}");

    char *json = malloc(MAX_JSON_SIZE);
    if (!json) {
        closedir(dr);
        return NULL;
    }

    #define JSON_CLOSE_RESERVE 4
    int n = snprintf(json, MAX_JSON_SIZE, "{\"pages\":[");
    size_t used = (n > 0) ? (size_t)n : 0;

    struct dirent *de;
    int first = 1;

    while ((de = readdir(dr)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        if (de->d_type == DT_DIR)
            continue;
        char resolved_section[16];
        if (!parse_section_from_filename(de->d_name, resolved_section,
            sizeof(resolved_section)))
            continue;
        if (strcmp(resolved_section, section) != 0)
            continue;
        char name[128];
        char *dot = strchr(de->d_name, '.');
        size_t name_len =
        dot ? (size_t)(dot - de->d_name) : strlen(de->d_name);
        if (name_len >= sizeof(name))
            name_len = sizeof(name) - 1;
        memcpy(name, de->d_name, name_len);
        name[name_len] = '\0';
        if (page_count < (sizeof(pages) / sizeof(pages[0]))) {
            pages[page_count] = strdup(name);
            if (pages[page_count])
                page_count++;
        }
    }
    closedir(dr);

    if (page_count > 1)
        qsort(pages, page_count, sizeof(pages[0]), compare_string_ptrs);

    for (size_t i = 0; i < page_count; i++) {
        size_t name_len  = strlen(pages[i]);
        size_t entry_len = (first ? 0 : 1) + 1 + name_len + 1;
        if (used + entry_len + JSON_CLOSE_RESERVE >= MAX_JSON_SIZE) {
            free(pages[i]);
            continue;
        }
        n = snprintf(json + used,
                     MAX_JSON_SIZE - used - JSON_CLOSE_RESERVE,
                     "%s\"%s\"", first ? "" : ",", pages[i]);
        free(pages[i]);
        if (n > 0) {
            used += (size_t)n;
            first = 0;
        }
    }
    (void)snprintf(json + used, MAX_JSON_SIZE - used, "]}");
    #undef JSON_CLOSE_RESERVE

    return json;
}

char *
man_get_page_metadata_json(const char *area, const char *section,
                           const char *name)
{
    char *filepath = resolve_man_path(name, section);
    if (!filepath)
        return strdup("{\"error\":\"Not found\"}");
    char *json = malloc(1024);
    if (!json) {
        free(filepath);
        return strdup("{\"error\":\"OOM\"}");
    }
    snprintf(json, 1024,
             "{\"name\":\"%s\",\"section\":\"%s\",\"area\":\"%s\","
             "\"path\":\"%s\"}",
             name, section, area, filepath);
    free(filepath);
    return json;
}

char *
man_api_search(const char *query)
{
    if (!is_valid_token(query))
        return strdup("");
    char *const argv[] = {
        "apropos",
        "-M", "/usr/share/man:/usr/local/man:/usr/X11R6/man",
        (char *)query, NULL
    };
    char *output = safe_popen_read_argv("/usr/bin/apropos", argv,
                                        MAX_OUTPUT_SIZE, 5, NULL);
    if (!output)
        return strdup("");
    return output;
}

char *
man_api_search_raw(const char *query)
{
    if (!query || strlen(query) < 2 || !is_valid_token(query))
        return strdup("");

    char *const argv[] = {
        "apropos",
        "-M", "/usr/share/man:/usr/local/man:/usr/X11R6/man",
        (char *)query, NULL
    };
    char *output = safe_popen_read_argv("/usr/bin/apropos", argv,
                                        1024 * 1024, 5, NULL);
    if (!output)
        output = strdup("");

    if (output[0] == '\0') {
        char *filepath = resolve_man_path(query, "1");
        if (!filepath)
            filepath = resolve_man_path(query, "8");

        if (filepath && filepath[0] != '\0') {
            char section[16] = {0};
            const char *base = strrchr(filepath, '/');
            base = base ? base + 1 : filepath;
            if (!parse_section_from_filename(base, section, sizeof(section)))
                strlcpy(section, "?", sizeof(section));
            char line[256];
            snprintf(line, sizeof(line), "%s (%s) - manual page",
                     query, section);
            free(output);
            output = strdup(line);
        }
        free(filepath);
    }
    return output;
}

/* =========================================================================
 * man_api_handler
 * ========================================================================= */

int
man_api_handler(http_request_t *req)
{
    char *json = NULL;

    const char *api_base = "/api/man";
    const char *path = strstr(req->url, api_base);
    if (!path)
        return http_send_error(req, 400, "Bad Request");
    path += strlen(api_base);

    const char *query_string = strchr(path, '?');
    size_t path_len =
    query_string ? (size_t)(query_string - path) : strlen(path);

    if (path_matches_endpoint(path, "/sections")) {
        json = man_get_sections_json();

    } else if (path_matches_endpoint(path, "/pages")) {
        char section[16] = {0};
        char area[16]    = "system";
        if (get_query_value(req->url, "section", section, sizeof(section))) {
            (void)get_query_value(req->url, "area", area, sizeof(area));
            json = man_get_section_pages_json(area, section);
        } else {
            json = strdup("{\"error\":\"Missing section parameter\"}");
        }

    } else if (path_matches_endpoint(path, "/resolve")) {
        char name_buf[64]    = {0};
        char section_buf[16] = {0};
        (void)get_query_value(req->url, "name",    name_buf,    sizeof(name_buf));
        (void)get_query_value(req->url, "section", section_buf, sizeof(section_buf));

        if (name_buf[0] == '\0' || !is_valid_token(name_buf) ||
            (section_buf[0] != '\0' && !is_valid_section(section_buf))) {
            json = strdup("{\"error\":\"name parameter required\"}");
            } else {
                char *filepath = NULL;
                if (section_buf[0] != '\0') {
                    filepath = resolve_man_path(name_buf, section_buf);
                } else {
                    static const char *probe_sections[] = {
                        "1", "8", "2", "3", "5", "7", "6", "4", "9", "3p"
                    };
                    for (size_t i = 0;
                         i < sizeof(probe_sections) / sizeof(probe_sections[0]);
                    i++) {
                        filepath = resolve_man_path(name_buf, probe_sections[i]);
                        if (filepath) {
                            strlcpy(section_buf, probe_sections[i],
                                    sizeof(section_buf));
                            break;
                        }
                    }
                }

                if (!filepath || filepath[0] == '\0') {
                    free(filepath);
                    json = strdup("{\"error\":\"not found\"}");
                } else {
                    const char *area = area_from_path(filepath);
                    char resolved_section[16] = {0};
                    const char *base = strrchr(filepath, '/');
                    base = base ? base + 1 : filepath;
                    if (!parse_section_from_filename(base, resolved_section,
                        sizeof(resolved_section))){
                        strlcpy(resolved_section, section_buf,
                                sizeof(resolved_section));
                    }

                    char *escaped_path = json_escape_string(filepath);
                    if (!escaped_path){
                        escaped_path = strdup("");
                    }

                    const char *sec_out =
                    resolved_section[0] ? resolved_section : section_buf;
                    int needed = snprintf(NULL, 0,
                                          "{\"name\":\"%s\","
                                          "\"section\":\"%s\","
                                          "\"area\":\"%s\","
                                          "\"path\":\"%s\"}",
                                          name_buf, sec_out, area, escaped_path);
                    if (needed > 0) {
                        json = malloc((size_t)needed + 1);
                        if (json)
                            snprintf(json, (size_t)needed + 1,
                                     "{\"name\":\"%s\","
                                     "\"section\":\"%s\","
                                     "\"area\":\"%s\","
                                     "\"path\":\"%s\"}",
                                     name_buf, sec_out, area, escaped_path);
                    }
                    free(escaped_path);
                    free(filepath);
                }
            }

    } else if (strncmp(path, "/search", 7) == 0) {
        const char *query = NULL;
        char query_buf[256] = {0};
        if (path[7] == '/') {
            query = path + 8;
        } else {
            if (get_query_value(req->url, "q", query_buf, sizeof(query_buf)))
                query = query_buf;
        }
        json = (query && *query != '\0') ? man_api_search_raw(query)
        : strdup("");

    } else {
        char area[32]    = {0};
        char section[16] = {0};
        char path_tmp[64] = {0};
        if (path_len < sizeof(path_tmp)) {
            strncpy(path_tmp, path, path_len);
            path_tmp[path_len] = '\0';
            if (sscanf(path_tmp, "/%31[^/]/%15s", area, section) == 2)
                json = man_get_section_pages_json(area, section);
        }
        if (!json)
            json = strdup("{\"error\":\"Unknown API endpoint or malformed path\"}");
    }

    if (!json)
        return http_send_error(req, 500, "Internal Server Error");

    http_response_t *resp = http_response_create();
    http_response_set_status(resp, 200);
    resp->content_type =
    (strstr(path, "/search") != NULL)
    ? "text/plain; charset=utf-8"
    : "application/json";
    http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
    http_response_set_body(resp, json, strlen(json), 1);

    int ret = http_response_send(req, resp);
    http_response_free(resp);
    return ret;
}

/* =========================================================================
 * Route registration
 * ========================================================================= */

int
man_module_attach_routes(struct router *r)
{
    if (router_register_prefix(r, "GET", "/man/", 2, man_render_handler) != 0)
        return -1;
    if (router_register_prefix(r, "GET", "/api/man", 0, man_api_handler) != 0)
        return -1;
    if (router_register(r, "GET", "/api/man/sections", man_api_handler) != 0)
        return -1;
    return 0;
}
