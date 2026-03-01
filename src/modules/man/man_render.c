#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <miniweb/core/conf.h>
#include <miniweb/core/config.h>
#include <miniweb/core/log.h>
#include <miniweb/http/utils.h>
#include <miniweb/modules/man.h>
#include "man_internal.h"

extern miniweb_conf_t config;
extern char config_static_dir[];

#define MAN_RENDER_CACHE_SHARDS 8
#define MAN_RENDER_CACHE_SLOTS 64
#define MAN_RENDER_CACHE_TTL 600

typedef struct {
    char key[192];
    char *body;
    size_t len;
    time_t inserted;
} man_render_slot_t;

typedef struct {
    man_render_slot_t slots[MAN_RENDER_CACHE_SLOTS];
    pthread_mutex_t lock;
} man_render_shard_t;

static man_render_shard_t g_man_cache[MAN_RENDER_CACHE_SHARDS];
static pthread_once_t g_man_cache_once = PTHREAD_ONCE_INIT;
static int g_man_cache_initialized = 0;

static sem_t g_mandoc_semaphore;
static pthread_once_t g_mandoc_sem_once = PTHREAD_ONCE_INIT;
static int g_mandoc_sem_initialized = 0;

static void
mandoc_semaphore_init(void)
{
    int max_concurrent = config.threads * 2;
    if (max_concurrent > 16)
        max_concurrent = 16;
    if (sem_init(&g_mandoc_semaphore, 0, max_concurrent) == 0)
        g_mandoc_sem_initialized = 1;
}

static void
man_render_cache_init(void)
{
    for (int i = 0; i < MAN_RENDER_CACHE_SHARDS; i++)
        pthread_mutex_init(&g_man_cache[i].lock, NULL);
    g_man_cache_initialized = 1;
}

static void
man_render_cache_key(const char *area, const char *section,
                     const char *page, const char *format,
                     char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s/%s.%s", area, section, page, format);
}

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
        if (!s->body || strcmp(s->key, key) != 0)
            continue;
        if ((now - s->inserted) > MAN_RENDER_CACHE_TTL) {
            free(s->body);
            s->body = NULL;
            s->len = 0;
            break;
        }
        char *copy = malloc(s->len + 1);
        if (copy) {
            memcpy(copy, s->body, s->len);
            copy[s->len] = '\0';
            *out_len = s->len;
            pthread_mutex_unlock(&shard->lock);
            return copy;
        }
        break;
    }
    pthread_mutex_unlock(&shard->lock);
    return NULL;
}

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
    int target = -1;
    time_t oldest = now + 1;

    for (int i = 0; i < MAN_RENDER_CACHE_SLOTS; i++) {
        man_render_slot_t *s = &shard->slots[i];

        if (s->body && strcmp(s->key, key) == 0) {
            free(s->body);
            s->body = malloc(len + 1);
            if (s->body) {
                memcpy(s->body, body, len);
                s->body[len] = '\0';
                s->len = len;
                s->inserted = now;
            }
            pthread_mutex_unlock(&shard->lock);
            return;
        }

        if (s->body == NULL) {
            target = i;
            break;
        }
        if (s->inserted < oldest) {
            oldest = s->inserted;
            target = i;
        }
    }

    if (target >= 0) {
        man_render_slot_t *s = &shard->slots[target];
        free(s->body);
        s->body = malloc(len + 1);
        if (s->body) {
            memcpy(s->body, body, len);
            s->body[len] = '\0';
            strlcpy(s->key, key, sizeof(s->key));
            s->len = len;
            s->inserted = now;
        }
    }

    pthread_mutex_unlock(&shard->lock);
}

void
man_strip_overstrike_ascii(char *text, size_t *len)
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

const char *
man_mime_for_format(const char *format)
{
    if (strcmp(format, "pdf") == 0)
        return "application/pdf";
    if (strcmp(format, "ps") == 0)
        return "application/postscript";
    if (strcmp(format, "md") == 0)
        return "text/markdown; charset=utf-8";
    if (strcmp(format, "txt") == 0)
        return "text/plain; charset=utf-8";
    return "text/html; charset=utf-8";
}

void
man_add_content_disposition_for_format(http_response_t *resp, const char *format,
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

char *
man_render_page(const char *area, const char *section, const char *page,
                const char *format, size_t *out_len)
{
    (void)area;
    pthread_once(&g_mandoc_sem_once, mandoc_semaphore_init);

    char *filepath = NULL;
    if (man_is_valid_section(section))
        filepath = man_resolve_path(page, section);

    if (!filepath || filepath[0] != '/') {
        free(filepath);
        return NULL;
    }

    const char *t_arg = "html";
    if (strcmp(format, "pdf") == 0)
        t_arg = "pdf";
    else if (strcmp(format, "ps") == 0)
        t_arg = "ps";
    else if (strcmp(format, "md") == 0)
        t_arg = "markdown";
    else if (strcmp(format, "txt") == 0)
        t_arg = "ascii";

    if (sem_wait(&g_mandoc_semaphore) != 0) {
        log_error("[MAN] Failed to acquire mandoc semaphore");
        free(filepath);
        return NULL;
    }

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
                                        MAN_MAX_OUTPUT_SIZE, 10, out_len);
    sem_post(&g_mandoc_semaphore);

    if (output && *out_len > 0 && strcmp(format, "txt") == 0)
        man_strip_overstrike_ascii(output, out_len);

    if (!output && strcmp(format, "md") == 0) {
        if (sem_wait(&g_mandoc_semaphore) == 0) {
            char *const argv_ascii[] = {"mandoc", "-T", "ascii", filepath, NULL};
            output = safe_popen_read_argv("/usr/bin/mandoc", argv_ascii,
                                          MAN_MAX_OUTPUT_SIZE, 10, out_len);
            sem_post(&g_mandoc_semaphore);
            if (output && *out_len > 0)
                man_strip_overstrike_ascii(output, out_len);
        }
    }

    free(filepath);
    return output;
}

int
man_render_handler(http_request_t *req)
{
    char area[32] = "system";
    char section[16] = "";
    char page[64] = "";
    char format[16] = "html";

    if (strncmp(req->url, "/man/", 5) != 0)
        return http_send_error(req, 400, "Invalid URL");

    const char *p = req->url + 5;
    char path_copy[256];
    strlcpy(path_copy, p, sizeof(path_copy));

    char *qs = strchr(path_copy, '?');
    if (qs)
        *qs = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(path_copy, "/", &saveptr);
    if (token)
        strlcpy(area, token, sizeof(area));
    token = strtok_r(NULL, "/", &saveptr);
    if (token)
        strlcpy(section, token, sizeof(section));
    token = strtok_r(NULL, "/", &saveptr);
    if (token) {
        char *dot = strrchr(token, '.');
        if (dot) {
            *dot = '\0';
            strlcpy(format, dot + 1, sizeof(format));
        }
        strlcpy(page, token, sizeof(page));
    }

    if (page[0] == '\0' || section[0] == '\0')
        return http_send_error(req, 400, "Missing section or page name");

    if (strcmp(format, "html") != 0 && strcmp(format, "pdf") != 0 &&
        strcmp(format, "ps") != 0 && strcmp(format, "md") != 0 &&
        strcmp(format, "txt") != 0)
        return http_send_error(req, 400, "Unsupported format");

    if (!man_is_valid_token(page) || !man_is_valid_section(section))
        return http_send_error(req, 400, "Invalid section or page name");

    char cache_rel[256] = {0};
    char cache_abs[512] = {0};
    int have_paths = (build_cache_paths(area, section, page, format,
                                        cache_rel, sizeof(cache_rel),
                                        cache_abs, sizeof(cache_abs)) == 0);

    char *response_body = NULL;
    size_t response_len = 0;

    response_body = man_render_cache_get(area, section, page, format, &response_len);
    if (response_body)
        goto send_response;

    if (have_paths) {
        struct stat st;
        if (stat(cache_abs, &st) == 0 && st.st_size > 0) {
            time_t now = time(NULL);
            if ((now - st.st_mtime) <= MAN_FS_CACHE_TTL_SEC) {
                int fd = open(cache_abs, O_RDONLY);
                if (fd >= 0) {
                    if (fstat(fd, &st) == 0 && st.st_size > 0) {
                        response_body = malloc((size_t)st.st_size + 1);
                        if (response_body) {
                            ssize_t nr = read(fd, response_body, (size_t)st.st_size);
                            if (nr == (ssize_t)st.st_size) {
                                response_body[nr] = '\0';
                                response_len = (size_t)nr;
                                close(fd);
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
            }
        }
    }

    response_body = man_render_page(area, section, page, format, &response_len);
    if (!response_body)
        return http_send_error(req, 404, "Manual page not found");

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

send_response:
    {
        http_response_t *resp = http_response_create();
        if (!resp) {
            free(response_body);
            return -1;
        }

        resp->content_type = man_mime_for_format(format);
        http_response_add_header(resp, "Cache-Control", "public, max-age=300");
        man_add_content_disposition_for_format(resp, format, page);
        http_response_set_body(resp, response_body, response_len, 1);

        int ret = http_response_send(req, resp);
        http_response_free(resp);
        return ret;
    }
}

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
    size_t path_len = query_string ? (size_t)(query_string - path) : strlen(path);

    if (man_path_matches_endpoint(path, "/sections")) {
        json = man_get_sections_json();

    } else if (man_path_matches_endpoint(path, "/pages")) {
        char section[16] = {0};
        char area[16] = "system";
        if (man_get_query_value(req->url, "section", section, sizeof(section))) {
            (void)man_get_query_value(req->url, "area", area, sizeof(area));
            json = man_get_section_pages_json(area, section);
        } else {
            json = strdup("{\"error\":\"Missing section parameter\"}");
        }

    } else if (man_path_matches_endpoint(path, "/resolve")) {
        char name_buf[64] = {0};
        char section_buf[16] = {0};
        (void)man_get_query_value(req->url, "name", name_buf, sizeof(name_buf));
        (void)man_get_query_value(req->url, "section", section_buf, sizeof(section_buf));

        if (name_buf[0] == '\0' || !man_is_valid_token(name_buf) ||
            (section_buf[0] != '\0' && !man_is_valid_section(section_buf))) {
            json = strdup("{\"error\":\"name parameter required\"}");
        } else {
            char *filepath = NULL;
            if (section_buf[0] != '\0') {
                filepath = man_resolve_path(name_buf, section_buf);
            } else {
                static const char *probe_sections[] = {
                    "1", "8", "2", "3", "5", "7", "6", "4", "9", "3p"
                };
                for (size_t i = 0; i < sizeof(probe_sections) / sizeof(probe_sections[0]); i++) {
                    filepath = man_resolve_path(name_buf, probe_sections[i]);
                    if (filepath) {
                        strlcpy(section_buf, probe_sections[i], sizeof(section_buf));
                        break;
                    }
                }
            }

            if (!filepath || filepath[0] == '\0') {
                free(filepath);
                json = strdup("{\"error\":\"not found\"}");
            } else {
                const char *area = man_area_from_path(filepath);
                char resolved_section[16] = {0};
                const char *base = strrchr(filepath, '/');
                base = base ? base + 1 : filepath;
                if (!man_parse_section_from_filename(base, resolved_section,
                    sizeof(resolved_section))) {
                    strlcpy(resolved_section, section_buf, sizeof(resolved_section));
                }

                char *escaped_path = json_escape_string(filepath);
                if (!escaped_path)
                    escaped_path = strdup("");

                const char *sec_out = resolved_section[0] ? resolved_section : section_buf;
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
        } else if (man_get_query_value(req->url, "q", query_buf, sizeof(query_buf))) {
            query = query_buf;
        }
        json = (query && *query != '\0') ? man_api_search_raw(query) : strdup("");

    } else {
        char area[32] = {0};
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
    if (!resp) {
        free(json);
        return http_send_error(req, 500, "Internal Server Error");
    }
    http_response_set_status(resp, 200);
    resp->content_type = (strstr(path, "/search") != NULL)
        ? "text/plain; charset=utf-8"
        : "application/json";
    http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
    http_response_set_body(resp, json, strlen(json), 1);

    int ret = http_response_send(req, resp);
    http_response_free(resp);
    return ret;
}

void
man_render_cache_cleanup(void)
{
    if (g_man_cache_initialized) {
        for (int si = 0; si < MAN_RENDER_CACHE_SHARDS; si++) {
            man_render_shard_t *shard = &g_man_cache[si];
            pthread_mutex_lock(&shard->lock);
            for (int i = 0; i < MAN_RENDER_CACHE_SLOTS; i++) {
                free(shard->slots[i].body);
                shard->slots[i].body = NULL;
                shard->slots[i].len = 0;
                shard->slots[i].key[0] = '\0';
            }
            pthread_mutex_unlock(&shard->lock);
        }
    }
    if (g_mandoc_sem_initialized)
        (void)sem_destroy(&g_mandoc_semaphore);
}
