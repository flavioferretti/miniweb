/* packages_module.c - pkg manager implementation with ring buffer cache */

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <miniweb/core/config.h>
#include <miniweb/core/heartbeat.h>
#include <miniweb/core/log.h>
#include <miniweb/http/handler.h>
#include <miniweb/http/utils.h>
#include <miniweb/modules/pkg_manager.h>
#include <miniweb/router/router.h>

#define PKG_JSON_MAX (1024 * 1024)
#define PKG_CMD_MAX_OUTPUT (8 * 1024 * 1024)
#define PKG_WHICH_TIMEOUT 60
#define PKG_CACHE_TTL_SEC 30  /* Cache TTL of 30 seconds */

/* Ring buffer for package queries - 2MB total for better cache capacity */
#define PKG_RING_BYTES (2 * 1024 * 1024)

/* Package query sample for ring buffer */
typedef struct {
	time_t ts;
	char endpoint[16];        /* "search", "info", "which", "files", "list" */
	char key[256];            /* query parameter or package name or file path */
	char *json;                /* Cached JSON response */
	size_t json_len;
	unsigned int hits;         /* Track popularity */
} PkgSample;

/* Ring buffer capacity based on total size */
#define PKG_RING_CAPACITY ((size_t)(PKG_RING_BYTES / sizeof(PkgSample)))

typedef struct {
	PkgSample *buf;
	size_t head;
	size_t count;
	pthread_mutex_t lock;
	unsigned int total_hits;
	unsigned int total_misses;
	time_t last_stats;
} PkgRing;

static PkgRing g_pkg_ring;
static pthread_once_t g_packages_once = PTHREAD_ONCE_INIT;
static int g_pkg_ring_ready = 0;

/* Logging macro */
#define LOG(...) do { \
if (config_verbose) \
	log_debug("[PKG] " __VA_ARGS__); \
} while (0)

/* =========================================================================
 * Ring buffer operations
 * ========================================================================= */

/** @brief pkg_ring_init function. */
static int
pkg_ring_init(PkgRing *r)
{
	r->buf = calloc(PKG_RING_CAPACITY, sizeof(PkgSample));
	if (!r->buf)
		return -1;

	r->head = 0;
	r->count = 0;
	r->total_hits = 0;
	r->total_misses = 0;
	r->last_stats = time(NULL);
	pthread_mutex_init(&r->lock, NULL);

	LOG("Ring buffer initialized: %zu slots, %.1f MB",
		PKG_RING_CAPACITY, (double)PKG_RING_BYTES / (1024 * 1024));
	return 0;
}

/** @brief pkg_ring_push function. */
static void
pkg_ring_push(PkgRing *r, const char *endpoint, const char *key,
			  const char *json, size_t json_len)
{
	if (!r->buf || !endpoint || !key || !json || json_len == 0)
		return;

	pthread_mutex_lock(&r->lock);

	/* Free old entry if exists */
	if (r->buf[r->head].json)
		free(r->buf[r->head].json);

	/* Store new sample */
	r->buf[r->head].ts = time(NULL);
	strlcpy(r->buf[r->head].endpoint, endpoint, sizeof(r->buf[r->head].endpoint));
	strlcpy(r->buf[r->head].key, key, sizeof(r->buf[r->head].key));
	r->buf[r->head].json = malloc(json_len + 1);
	if (r->buf[r->head].json) {
		memcpy(r->buf[r->head].json, json, json_len);
		r->buf[r->head].json[json_len] = '\0';
		r->buf[r->head].json_len = json_len;
		r->buf[r->head].hits = 1;
	}

	r->head = (r->head + 1) % PKG_RING_CAPACITY;
	if (r->count < PKG_RING_CAPACITY)
		r->count++;

	pthread_mutex_unlock(&r->lock);
}

/**
 * @brief Find a recent sample in the ring buffer
 * @param endpoint The endpoint type (search, info, etc.)
 * @param key The query key
 * @param max_age_sec Maximum age in seconds
 * @return Newly allocated JSON string if found, NULL otherwise
 */
static char *
pkg_ring_find(PkgRing *r, const char *endpoint, const char *key, int max_age_sec)
{
	if (!r->buf || r->count == 0)
		return NULL;

	time_t now = time(NULL);
	char *result = NULL;
	int found_idx = -1;
	time_t newest_ts = 0;

	pthread_mutex_lock(&r->lock);

	/* Search from newest to oldest */
	size_t start = (r->head + PKG_RING_CAPACITY - 1) % PKG_RING_CAPACITY;
	for (size_t i = 0; i < r->count; i++) {
		size_t idx = (start + PKG_RING_CAPACITY - i) % PKG_RING_CAPACITY;
		PkgSample *s = &r->buf[idx];

		if (s->json &&
			strcmp(s->endpoint, endpoint) == 0 &&
			strcmp(s->key, key) == 0 &&
			(now - s->ts) <= max_age_sec) {

			/* Found a match - take the newest one */
			if (s->ts > newest_ts) {
				newest_ts = s->ts;
				found_idx = idx;
			}
			}
	}

	if (found_idx >= 0) {
		PkgSample *s = &r->buf[found_idx];
		result = malloc(s->json_len + 1);
		if (result) {
			memcpy(result, s->json, s->json_len);
			result[s->json_len] = '\0';
			s->hits++;
			r->total_hits++;
			LOG("Ring cache HIT for %s:%s (age=%lds, hits=%u)",
				endpoint, key, (long)(now - s->ts), s->hits);
		}
	}

	if (!result) {
		r->total_misses++;
		LOG("Ring cache MISS for %s:%s", endpoint, key);
	}

	/* Log stats every 1000 requests */
	if ((r->total_hits + r->total_misses) % 1000 == 0 &&
		now - r->last_stats > 5) {
		float hit_ratio = r->total_hits * 100.0 / (r->total_hits + r->total_misses);
	LOG("Cache stats: hits=%u, misses=%u, hit ratio=%.1f%%",
		r->total_hits, r->total_misses, hit_ratio);
	r->last_stats = now;
		}

		pthread_mutex_unlock(&r->lock);

		return result;
}

/** @brief pkg_ring_free function. */
static void
pkg_ring_free(PkgRing *r)
{
	if (!r->buf)
		return;

	pthread_mutex_lock(&r->lock);
	for (size_t i = 0; i < PKG_RING_CAPACITY; i++) {
		free(r->buf[i].json);
	}
	free(r->buf);
	r->buf = NULL;
	r->count = 0;
	r->head = 0;
	pthread_mutex_unlock(&r->lock);
	pthread_mutex_destroy(&r->lock);
}

/* =========================================================================
 * Package query functions with ring buffer integration
 * ========================================================================= */

/** @brief is_safe_pkg_name function. */
static int
is_safe_pkg_name(const char *name)
{
	if (!name || *name == '\0')
		return 0;

	for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
		if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-' && *p != '+')
			return 0;
	}
	return 1;
}

/** @brief url_decode_into function. */
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

/** @brief get_query_value function. */
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
		const char *eq = strchr(entry, '=');
		const char *amp = strchr(entry, '&');

		if (!amp)
			amp = entry + strlen(entry);
		if (!eq || eq > amp) {
			qs = (*amp == '&') ? amp + 1 : amp;
			continue;
		}

		if ((size_t)(eq - entry) == key_len &&
			strncmp(entry, key, key_len) == 0) {
			char encoded[1024];
			size_t encoded_len = (size_t)(amp - (eq + 1));

			if (encoded_len >= sizeof(encoded)){
				return 0;

			}

			memcpy(encoded, eq + 1, encoded_len);
			encoded[encoded_len] = '\0';

			if (url_decode_into(encoded, out, out_size) != 0){
				return 0;
			}

			return 1;
		}
		qs = (*amp == '&') ? amp + 1 : amp;
	}

	return 0;
}

/** @brief path_matches_endpoint function. */
static int
path_matches_endpoint(const char *path, const char *prefix)
{
	size_t len;

	if (!path || !prefix)
		return 0;

	len = strlen(prefix);
	if (strncmp(path, prefix, len) != 0)
		return 0;

	return path[len] == '\0' || path[len] == '?';
}

/** @brief is_safe_path function. */
static int
is_safe_path(const char *path)
{
	if (!path || path[0] != '/')
		return 0;
	for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
		if (*p < 0x20 || *p == 0x7f)
			return 0;
		if (*p == ';' || *p == '|' || *p == '&' || *p == '`' ||
			*p == '$' || *p == '>' || *p == '<' || *p == '\\' ||
			*p == '"' || *p == '\'')
			return 0;
	}
	return 1;
}

/** @brief trim_ascii_whitespace function. */
static void
trim_ascii_whitespace(char **start)
{
	char *s;
	char *end;

	if (!start || !*start)
		return;

	s = *start;
	while (*s == ' ' || *s == '\t')
		s++;

	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
		*--end = '\0';

	*start = s;
}

/** @brief make_raw_json function. */
static char *
make_raw_json(const char *output)
{
	if (!output || output[0] == '\0')
		return strdup("{\"raw\":\"\",\"found\":false}");

	/* trim trailing whitespace */
	char *copy = strdup(output);
	if (!copy)
		return NULL;

	size_t len = strlen(copy);
	while (len > 0 && (copy[len - 1] == '\n' || copy[len - 1] == '\r' ||
		copy[len - 1] == ' ' || copy[len - 1] == '\t'))
		copy[--len] = '\0';

	char *esc = json_escape_string(copy);
	free(copy);
	if (!esc)
		return NULL;

	size_t jlen = strlen(esc) + 32;
	char *json = malloc(jlen);
	if (!json) {
		free(esc);
		return NULL;
	}
	snprintf(json, jlen, "{\"raw\":\"%s\",\"found\":true}", esc);
	free(esc);
	return json;
}

/* Bootstrap function called via pthread_once */
/** @brief packages_cache_bootstrap function. */
static void
packages_cache_bootstrap(void)
{
	LOG("Initializing package cache ring buffer (2MB)");

	if (pkg_ring_init(&g_pkg_ring) != 0) {
		LOG("Failed to initialize package ring buffer");
		return;
	}
	g_pkg_ring_ready = 1;

	/* No heartbeat needed - ring buffer is populated on demand */

	LOG("Package cache initialized successfully");
}

/* =========================================================================
 * Public API functions with ring buffer caching
 * ========================================================================= */

/** @brief pkg_search_json function. */
char *
pkg_search_json(const char *query)
{
	char *output = NULL;
	char *json = NULL;
	size_t offset = 0;
	int first = 1;
	int count = 0;

	LOG("pkg_search_json called with query: '%s'", query);

	if (!query || strlen(query) < 1) {
		LOG("Empty query");
		return strdup("{\"query\":\"\",\"packages\":[]}");
	}

	/* Ensure cache is initialized */
	pthread_once(&g_packages_once, packages_cache_bootstrap);

	/* Try ring buffer cache first */
	if (g_pkg_ring_ready) {
		json = pkg_ring_find(&g_pkg_ring, "search", query, PKG_CACHE_TTL_SEC);
		if (json) {
			LOG("Returning cached result for '%s'", query);
			return json;
		}
	}

	/* Try pkg_info -Q */
	if (is_safe_pkg_name(query)) {
		char *const argv[] = {"pkg_info", "-Q", (char *)query, NULL};
		LOG("Executing: pkg_info -Q %s", query);
		output = safe_popen_read_argv("/usr/sbin/pkg_info", argv,
									  PKG_CMD_MAX_OUTPUT, 5, NULL);
	}

	/* Fallback to pkg_info -q | grep */
	if (!output || output[0] == '\0') {
		LOG("pkg_info -Q failed, trying grep approach");
		free(output);

		char *list_output = NULL;
		char *const argv_list[] = {"pkg_info", "-q", NULL};
		list_output = safe_popen_read_argv("/usr/sbin/pkg_info", argv_list,
										   PKG_CMD_MAX_OUTPUT, 5, NULL);

		if (list_output) {
			char *saveptr = NULL;
			char *line = strtok_r(list_output, "\n", &saveptr);
			size_t total = 0;
			output = malloc(PKG_CMD_MAX_OUTPUT);
			if (output) {
				output[0] = '\0';
				while (line) {
					if (strstr(line, query)) {
						size_t len = strlen(line);
						if (total + len + 2 < PKG_CMD_MAX_OUTPUT) {
							if (total > 0) {
								strlcat(output, "\n", PKG_CMD_MAX_OUTPUT);
								total++;
							}
							strlcat(output, line, PKG_CMD_MAX_OUTPUT);
							total += len;
						}
					}
					line = strtok_r(NULL, "\n", &saveptr);
				}
				LOG("grep found %zu bytes", total);
			}
			free(list_output);
		}
	}

	/* Last resort: direct popen */
	if (!output || output[0] == '\0') {
		LOG("Trying popen directly");
		free(output);

		char cmd[256];
		snprintf(cmd, sizeof(cmd), "pkg_info -Q %s 2>&1", query);
		FILE *fp = popen(cmd, "r");
		if (fp) {
			char buf[1024];
			size_t total = 0;
			output = malloc(PKG_CMD_MAX_OUTPUT);
			if (output) {
				output[0] = '\0';
				while (fgets(buf, sizeof(buf), fp) &&
					total < PKG_CMD_MAX_OUTPUT - 1) {
					size_t len = strlen(buf);
				if (total + len < PKG_CMD_MAX_OUTPUT - 1) {
					strlcat(output, buf, PKG_CMD_MAX_OUTPUT);
					total += len;
				}
					}
					LOG("popen returned %zu bytes", total);
			}
			pclose(fp);
		}
	}

	if (!output || output[0] == '\0') {
		LOG("All methods failed, returning empty");
		free(output);
		return strdup("{\"query\":\"\",\"packages\":[]}");
	}

	LOG("Raw output: '%s'", output);

	/* Build JSON */
	json = malloc(PKG_JSON_MAX);
	if (!json) {
		free(output);
		return strdup("{\"query\":\"\",\"packages\":[]}");
	}

	char *query_escaped = json_escape_string(query);
	offset += snprintf(json + offset, PKG_JSON_MAX - offset,
					   "{\"query\":\"%s\",\"packages\":[",
					query_escaped ? query_escaped : "");
	free(query_escaped);

	char *saveptr = NULL;
	char *line = strtok_r(output, "\n", &saveptr);

	while (line && offset < PKG_JSON_MAX - 256) {
		line[strcspn(line, "\r")] = '\0';

		trim_ascii_whitespace(&line);

		if (*line != '\0' && strlen(line) > 1) {
			char *escaped_line = json_escape_string(line);
			offset += snprintf(json + offset, PKG_JSON_MAX - offset,
							   "%s\"%s\"", first ? "" : ",",
					  escaped_line ? escaped_line : "");
			free(escaped_line);
			first = 0;
			count++;
		}
		line = strtok_r(NULL, "\n", &saveptr);
	}

	offset += snprintf(json + offset, PKG_JSON_MAX - offset, "]}");
	free(output);

	LOG("Final JSON with %d packages", count);

	/* Store in ring buffer cache */
	if (g_pkg_ring_ready) {
		pkg_ring_push(&g_pkg_ring, "search", query, json, strlen(json));
	}

	return json;
}

/** @brief pkg_info_json function. */
char *
pkg_info_json(const char *package_name)
{
	if (!is_safe_pkg_name(package_name)) {
		return strdup("{\"found\":false,\"raw\":\"invalid package name\"}");
	}

	/* Ensure cache is initialized */
	pthread_once(&g_packages_once, packages_cache_bootstrap);

	/* Try ring buffer cache first */
	if (g_pkg_ring_ready) {
		char *json = pkg_ring_find(&g_pkg_ring, "info", package_name, PKG_CACHE_TTL_SEC);
		if (json) {
			LOG("Returning cached info for '%s'", package_name);
			return json;
		}
	}

	char *const argv[] = {"pkg_info", (char *)package_name, NULL};
	char *output = safe_popen_read_argv("/usr/sbin/pkg_info", argv,
										PKG_CMD_MAX_OUTPUT, 5, NULL);
	char *json = make_raw_json(output);
	free(output);

	if (json && g_pkg_ring_ready) {
		pkg_ring_push(&g_pkg_ring, "info", package_name, json, strlen(json));
	}

	return json ? json : strdup("{\"found\":false,\"raw\":\"\"}");
}

/** @brief pkg_files_json function. */
char *
pkg_files_json(const char *package_name)
{
	if (!is_safe_pkg_name(package_name))
		return strdup("{\"error\":\"invalid package name\"}");

	/* Ensure cache is initialized */
	pthread_once(&g_packages_once, packages_cache_bootstrap);

	/* Try ring buffer cache first */
	if (g_pkg_ring_ready) {
		char *json = pkg_ring_find(&g_pkg_ring, "files", package_name, PKG_CACHE_TTL_SEC);
		if (json) {
			LOG("Returning cached files for '%s'", package_name);
			return json;
		}
	}

	char *const argv[] = {"pkg_info", "-L", (char *)package_name, NULL};
	char *output = safe_popen_read_argv("/usr/sbin/pkg_info", argv,
										PKG_CMD_MAX_OUTPUT, 5, NULL);

	char *escaped_name = json_escape_string(package_name);
	if (!escaped_name)
		escaped_name = strdup("");

	if (!output) {
		char *json = malloc(strlen(escaped_name) + 80);
		if (!json) {
			free(escaped_name);
			return NULL;
		}
		snprintf(json, strlen(escaped_name) + 80,
				 "{\"package\":\"%s\",\"files\":[]}", escaped_name);
		free(escaped_name);
		return json;
	}

	char *json = malloc(PKG_JSON_MAX);
	if (!json) {
		free(escaped_name);
		free(output);
		return NULL;
	}

	size_t offset = 0;
	offset += snprintf(json + offset, PKG_JSON_MAX - offset,
					   "{\"package\":\"%s\",\"files\":[", escaped_name);
	free(escaped_name);

	int first = 1;
	char *saveptr = NULL;
	char *line = strtok_r(output, "\n", &saveptr);
	while (line && offset < PKG_JSON_MAX - 256) {
		line[strcspn(line, "\r")] = '\0';
		/* Only include lines that are absolute paths */
		if (line[0] == '/') {
			char *escaped_line = json_escape_string(line);
			offset += snprintf(json + offset, PKG_JSON_MAX - offset,
							   "%s\"%s\"", first ? "" : ",",
					  escaped_line ? escaped_line : "");
			free(escaped_line);
			first = 0;
		}
		line = strtok_r(NULL, "\n", &saveptr);
	}
	free(output);

	offset += snprintf(json + offset, PKG_JSON_MAX - offset, "]}");

	if (g_pkg_ring_ready) {
		pkg_ring_push(&g_pkg_ring, "files", package_name, json, strlen(json));
	}

	return json;
}

/** @brief pkg_list_json function. */
char *
pkg_list_json(void)
{
	/* Ensure cache is initialized */
	pthread_once(&g_packages_once, packages_cache_bootstrap);

	/* Try ring buffer cache first */
	if (g_pkg_ring_ready) {
		char *json = pkg_ring_find(&g_pkg_ring, "list", "all", PKG_CACHE_TTL_SEC);
		if (json) {
			LOG("Serving cached package list");
			return json;
		}
	}

	LOG("Cache miss, generating fresh package list");

	char *const argv[] = {"pkg_info", "-qmz", NULL};
	char *output = safe_popen_read_argv("/usr/sbin/pkg_info", argv,
										PKG_CMD_MAX_OUTPUT, 5, NULL);

	char *json = malloc(PKG_JSON_MAX);
	if (!json) {
		free(output);
		return strdup("{\"packages\":[]}");
	}

	size_t offset = 0;
	offset += snprintf(json + offset, PKG_JSON_MAX - offset, "{\"packages\":[");

	int first = 1;
	if (output) {
		char *saveptr = NULL;
		char *line = strtok_r(output, "\n", &saveptr);
		while (line && offset < PKG_JSON_MAX - 256) {
			line[strcspn(line, "\r")] = '\0';
			if (*line != '\0') {
				char *escaped_line = json_escape_string(line);
				offset += snprintf(json + offset, PKG_JSON_MAX - offset,
								   "%s\"%s\"", first ? "" : ",",
					   escaped_line ? escaped_line : "");
				free(escaped_line);
				first = 0;
			}
			line = strtok_r(NULL, "\n", &saveptr);
		}
	}

	offset += snprintf(json + offset, PKG_JSON_MAX - offset, "]}");
	free(output);

	/* Store in ring buffer cache */
	if (g_pkg_ring_ready) {
		pkg_ring_push(&g_pkg_ring, "list", "all", json, strlen(json));
	}

	return json;
}

/** @brief pkg_which_json function. */
char *
pkg_which_json(const char *file_path)
{
	if (!is_safe_path(file_path)) {
		return strdup("{\"found\":false,\"raw\":\"path must be absolute and "
		"contain no shell metacharacters\"}");
	}

	/* Ensure cache is initialized */
	pthread_once(&g_packages_once, packages_cache_bootstrap);

	/* Try ring buffer cache first */
	if (g_pkg_ring_ready) {
		char *json = pkg_ring_find(&g_pkg_ring, "which", file_path, PKG_CACHE_TTL_SEC);
		if (json) {
			LOG("Returning cached which for '%s'", file_path);
			return json;
		}
	}

	char *const argv_w[] = {"pkg_info", "-W", (char *)file_path, NULL};
	char *output = safe_popen_read_argv("/usr/sbin/pkg_info", argv_w,
										PKG_CMD_MAX_OUTPUT, PKG_WHICH_TIMEOUT, NULL);
	if (!output || output[0] == '\0') {
		free(output);
		char *const argv_e[] = {"pkg_info", "-E", (char *)file_path, NULL};
		output = safe_popen_read_argv("/usr/sbin/pkg_info", argv_e,
									  PKG_CMD_MAX_OUTPUT, PKG_WHICH_TIMEOUT, NULL);
	}
	char *json = make_raw_json(output);
	free(output);

	if (json && g_pkg_ring_ready) {
		pkg_ring_push(&g_pkg_ring, "which", file_path, json, strlen(json));
	}

	return json ? json : strdup("{\"found\":false,\"raw\":\"\"}");
}

/** @brief pkg_api_handler function. */
int
pkg_api_handler(http_request_t *req)
{
	char *json = NULL;
	char value[1024];
	const char *base = "/api/packages";
	const char *path = req->url;

	LOG("Handling request: %s", req->url);

	if (!path || strncmp(path, base, strlen(base)) != 0) {
		return http_send_error(req, 400, "Bad Request");
	}

	path += strlen(base);

	if (path_matches_endpoint(path, "/search")) {
		if (!get_query_value(req->url, "q", value, sizeof(value)))
			return http_send_error(req, 400, "Missing q parameter");
		LOG("Search query: %s", value);
		json = pkg_search_json(value);
	} else if (path_matches_endpoint(path, "/info")) {
		if (!get_query_value(req->url, "name", value, sizeof(value)))
			return http_send_error(req, 400, "Missing name parameter");
		LOG("Info for: %s", value);
		json = pkg_info_json(value);
	} else if (path_matches_endpoint(path, "/which")) {
		if (!get_query_value(req->url, "path", value, sizeof(value)))
			return http_send_error(req, 400, "Missing path parameter");
		LOG("Which for path: %s", value);
		json = pkg_which_json(value);
	} else if (path_matches_endpoint(path, "/files")) {
		if (!get_query_value(req->url, "name", value, sizeof(value)))
			return http_send_error(req, 400, "Missing name parameter");
		LOG("Files for: %s", value);
		json = pkg_files_json(value);
	} else if (path_matches_endpoint(path, "/list")) {
		LOG("Listing all packages");
		json = pkg_list_json();
	} else {
		return http_send_error(req, 404, "Unknown packages endpoint");
	}

	if (!json) {
		LOG("Failed to generate JSON");
		return http_send_error(req, 500, "Unable to query package manager");
	}

	LOG("Generated JSON (%zu bytes)", strlen(json));

	http_response_t *resp = http_response_create();
	resp->status_code = 200;
	resp->content_type = "application/json";
	http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
	http_response_set_body(resp, json, strlen(json), 1);

	int ret = http_response_send(req, resp);
	http_response_free(resp);

	LOG("Response sent, ret=%d", ret);
	return ret;
}

/** @brief packages_module_attach_routes function. */
int
packages_module_attach_routes(struct router *r)
{
	/* Register the prefix route for all packages endpoints. */
	if (router_register_prefix(r, "GET", "/api/packages", 0,
		pkg_api_handler) != 0)
		return -1;

	return 0;
}

/* Optional cleanup function */
/** @brief packages_cache_cleanup function. */
void
packages_cache_cleanup(void)
{
	LOG("Cleaning up package caches");
	pkg_ring_free(&g_pkg_ring);
}
