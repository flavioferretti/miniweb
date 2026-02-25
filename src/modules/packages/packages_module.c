/* packages_module.c - pkg manager implementation */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include <miniweb/router/router.h>
#include <miniweb/http/handler.h>
#include <miniweb/http/utils.h>
#include <miniweb/modules/pkg_manager.h>
#include <miniweb/core/log.h>

#define PKG_JSON_MAX (1024 * 1024)
#define PKG_CMD_MAX_OUTPUT (8 * 1024 * 1024)
#define PKG_WHICH_TIMEOUT                                                      \
	60 /* pkg_info -E scans all packages: can take ~30s                    \
	    */
#define PKG_SEARCH_CACHE_SIZE 64
#define PKG_SEARCH_CACHE_TTL_SEC 30

typedef struct {
	char *query;
	char *json;
	time_t created_at;
	time_t last_used_at;
} pkg_search_cache_entry_t;

static pkg_search_cache_entry_t g_pkg_search_cache[PKG_SEARCH_CACHE_SIZE];
static pthread_mutex_t g_pkg_search_cache_lock = PTHREAD_MUTEX_INITIALIZER;

//static int compare_pkg_names(const void *a, const void *b);
//static int collect_pkg_name_from_line(char *line, char **out_name);

static void
pkg_search_cache_store_locked(const char *query, const char *json, time_t now)
{
	int slot = -1;
	time_t oldest = now;

	for (int i = 0; i < PKG_SEARCH_CACHE_SIZE; i++) {
		if (g_pkg_search_cache[i].query &&
		    strcmp(g_pkg_search_cache[i].query, query) == 0) {
			slot = i;
			break;
		}
		if (!g_pkg_search_cache[i].query) {
			slot = i;
			break;
		}
		if (g_pkg_search_cache[i].last_used_at <= oldest) {
			oldest = g_pkg_search_cache[i].last_used_at;
			slot = i;
		}
	}

	if (slot < 0)
		return;

	free(g_pkg_search_cache[slot].query);
	free(g_pkg_search_cache[slot].json);
	g_pkg_search_cache[slot].query = strdup(query);
	g_pkg_search_cache[slot].json = strdup(json);
	if (!g_pkg_search_cache[slot].query || !g_pkg_search_cache[slot].json) {
		free(g_pkg_search_cache[slot].query);
		free(g_pkg_search_cache[slot].json);
		memset(&g_pkg_search_cache[slot], 0,
		       sizeof(g_pkg_search_cache[slot]));
		return;
	}
	g_pkg_search_cache[slot].created_at = now;
	g_pkg_search_cache[slot].last_used_at = now;
}

/**
 * @brief Is safe pkg name.
 * @param name Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
static int
is_safe_pkg_name(const char *name)
{
	if (!name || *name == '\0')
		return 0;

	for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
		if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-' &&
		    *p != '+')
			return 0;
	}
	return 1;
}

/**
 * @brief Url decode into.
 * @param src Parameter used by this function.
 * @param dst Parameter used by this function.
 * @param dst_size Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
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

/**
 * @brief Get query value.
 * @param url Request URL path.
 * @param key Parameter used by this function.
 * @param out Output pointer for parsed or generated value.
 * @param out_size Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
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

			if (encoded_len >= sizeof(encoded))
				return 0;

			memcpy(encoded, eq + 1, encoded_len);
			encoded[encoded_len] = '\0';

			if (url_decode_into(encoded, out, out_size) != 0)
				return 0;
			return 1;
		}

		qs = (*amp == '&') ? amp + 1 : amp;
	}

	return 0;
}

char *
pkg_search_json(const char *query)
{
	char *output = NULL;
	char *json = NULL;
	size_t offset = 0;
	int first = 1;
	int count = 0;

	log_debug("[PKG] pkg_search_json called with query: '%s'", query);

	if (!query || strlen(query) < 1) {
		log_debug("[PKG] Empty query");
		return strdup("{\"query\":\"\",\"packages\":[]}");
	}

	/* Prova prima con pkg_info -Q */
	if (is_safe_pkg_name(query)) {
		char *const argv[] = {"pkg_info", "-Q", (char *)query, NULL};
		log_debug("[PKG] Executing: pkg_info -Q %s", query);
		output = safe_popen_read_argv("/usr/sbin/pkg_info", argv,
									  PKG_CMD_MAX_OUTPUT, 5, NULL);
	}

	/* Se pkg_info -Q fallisce, prova con which per vedere se pkg_info esiste */
	if (!output) {
		log_debug("[PKG] pkg_info -Q failed, checking if pkg_info exists");
		if (access("/usr/sbin/pkg_info", X_OK) == 0) {
			log_debug("[PKG] /usr/sbin/pkg_info is executable");
		} else {
			log_debug("[PKG] /usr/sbin/pkg_info is NOT executable: %s",
					  strerror(errno));
		}

		/* Prova a eseguire un comando semplice per test */
		char *const test_argv[] = {"ls", "-la", "/usr/sbin/pkg_info", NULL};
		char *test_out = safe_popen_read_argv("/bin/ls", test_argv, 1024, 5, NULL);
		if (test_out) {
			log_debug("[PKG] ls test output: %s", test_out);
			free(test_out);
		} else {
			log_debug("[PKG] ls test failed");
		}
	}

	/* Se ancora fallisce, prova con popen diretto */
	if (!output) {
		log_debug("[PKG] Trying popen directly");
		char cmd[256];
		snprintf(cmd, sizeof(cmd), "pkg_info -Q %s 2>&1", query);
		FILE *fp = popen(cmd, "r");
		if (fp) {
			char buf[1024];
			size_t total = 0;
			output = malloc(PKG_CMD_MAX_OUTPUT);
			if (output) {
				while (fgets(buf, sizeof(buf), fp) && total < PKG_CMD_MAX_OUTPUT - 1) {
					size_t len = strlen(buf);
					memcpy(output + total, buf, len);
					total += len;
				}
				output[total] = '\0';
				log_debug("[PKG] popen returned %zu bytes", total);
				if (total > 0) {
					log_debug("[PKG] popen output: %s", output);
				}
			}
			pclose(fp);
		} else {
			log_debug("[PKG] popen failed: %s", strerror(errno));
		}
	}

	if (!output) {
		log_debug("[PKG] All methods failed, returning empty");
		return strdup("{\"query\":\"\",\"packages\":[]}");
	}

	log_debug("[PKG] Raw output length: %zu", strlen(output));
	if (strlen(output) < 500) {
		log_debug("[PKG] Raw output: '%s'", output);
	}

	/* Costruisci JSON */
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

		/* Rimuovi " (installed)" se presente */
		char *tag = strstr(line, " (installed)");
		if (tag) *tag = '\0';

		/* Trim */
		while (*line == ' ' || *line == '\t') line++;
		char *end = line + strlen(line) - 1;
		while (end > line && (*end == ' ' || *end == '\t')) *end-- = '\0';

		if (*line != '\0' && strlen(line) > 1) {
			char *escaped_line = json_escape_string(line);
			offset += snprintf(json + offset, PKG_JSON_MAX - offset,
							   "%s\"%s\"", first ? "" : ",",
					  escaped_line ? escaped_line : "");
			free(escaped_line);
			first = 0;
			count++;
			log_debug("[PKG] Added package: %s", line);
		}
		line = strtok_r(NULL, "\n", &saveptr);
	}

	offset += snprintf(json + offset, PKG_JSON_MAX - offset, "]}");
	free(output);

	log_debug("[PKG] Final JSON (%d packages): %s", count, json);
	return json;
}

/* Validate filesystem path for pkg_info -W: must be absolute, no shell chars */
/**
 * @brief Is safe path.
 * @param path Request or filesystem path to evaluate.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
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

/* Build {"raw":"<escaped output>"} — same idea as Node's
 * res.end(JSON.stringify({ info: stdout.trim() })) */
/**
 * @brief Make raw json.
 * @param output Rendered output buffer pointer.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
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

/**
 * @brief Pkg info json.
 * @param package_name Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
char *
pkg_info_json(const char *package_name)
{
	if (!is_safe_pkg_name(package_name))
		return strdup(
		    "{\"found\":false,\"raw\":\"invalid package name\"}");

	char *const argv[] = {"pkg_info", (char *)package_name, NULL};
	char *output = safe_popen_read_argv("/usr/sbin/pkg_info", argv,
					    PKG_CMD_MAX_OUTPUT, 5, NULL);
	char *json = make_raw_json(output);
	free(output);
	return json ? json : strdup("{\"found\":false,\"raw\":\"\"}");
}

/**
 * @brief Pkg files json.
 * @param package_name Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
char *
pkg_files_json(const char *package_name)
{
	if (!is_safe_pkg_name(package_name))
		return strdup("{\"error\":\"invalid package name\"}");

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
		/*
		 * pkg_info -L output starts with header lines like:
		 *   "Information for inst:<pkg>:"
		 *   ""
		 *   "Files:"
		 * Only include lines that are absolute paths (start with '/').
		 */
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
	return json;
}

/**
 * Pkg list json.
 * Ritorna la lista di tutti i package installati
 */
/**
 * Pkg list json.
 */
char *
pkg_list_json(void)
{
	char *const argv[] = {"pkg_info", "-q", NULL};
	char *output = safe_popen_read_argv("/usr/sbin/pkg_info", argv,
										PKG_CMD_MAX_OUTPUT, 5, NULL);

	char *json = malloc(PKG_JSON_MAX);
	if (!json) {
		free(output);
		return strdup("{\"packages\":[]}");
	}

	size_t offset = 0;
	offset +=
	snprintf(json + offset, PKG_JSON_MAX - offset, "{\"packages\":[");

	int first = 1;
	if (output) {
		char *saveptr = NULL;
		char *line = strtok_r(output, "\n", &saveptr);
		while (line && offset < PKG_JSON_MAX - 256) {
			line[strcspn(line, "\r")] = '\0';
			if (*line != '\0') {
				char *escaped_line = json_escape_string(line);
				offset += snprintf(
					json + offset, PKG_JSON_MAX - offset,
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
	return json;
}

/**
 * @brief Pkg which json.
 * @param file_path Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
 */
char *
pkg_which_json(const char *file_path)
{
	if (!is_safe_path(file_path))
		return strdup(
		    "{\"found\":false,\"raw\":\"path must be absolute and "
		    "contain no shell metacharacters\"}");

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
	return json ? json : strdup("{\"found\":false,\"raw\":\"\"}");
}

/**
 * @brief Return non-zero when the request path matches an endpoint exactly.
 * @param path Request URL path segment after /api/packages.
 * @param prefix Endpoint path segment to match.
 * @return 1 when @p path matches @p prefix followed by end or query string.
 */
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

/**
 * @brief qsort comparator for package names.
 * @param a Pointer to left element.
 * @param b Pointer to right element.
 * @return strcmp-style ordering result.
 */
// static int
// compare_pkg_names(const void *a, const void *b)
// {
// 	const char *const *left = a;
// 	const char *const *right = b;
//
// 	return strcmp(*left, *right);
// }

// static int
// collect_pkg_name_from_line(char *line, char **out_name)
// {
// 	char *candidate;
//
// 	if (!line || !out_name)
// 		return 0;
//
// 	line[strcspn(line, "\r")] = '\0';
// 	if (*line == '\0')
// 		return 0;
//
// 	candidate = line;
// 	if (strncmp(line, "Information for inst:", 21) == 0) {
// 		candidate = line + 21;
// 		char *colon = strchr(candidate, ':');
// 		if (colon)
// 			*colon = '\0';
// 	}
//
// 	if (*candidate == '\0' || strchr(candidate, ':') ||
// 	    strpbrk(candidate, " \t"))
// 		return 0;
//
// 	if (!is_safe_pkg_name(candidate))
// 		return 0;
//
// 	*out_name = strdup(candidate);
// 	return *out_name != NULL;
// }

/**
 * Pkg api handler.
 * Endpoints supportati:
 *   /api/packages/search?q=QUERY
 *   /api/packages/info?name=PACKAGE
 *   /api/packages/which?path=PATH
 *   /api/packages/files?name=PACKAGE
 *   /api/packages/list
 */
/**
 * Pkg api handler.
 */
int
pkg_api_handler(http_request_t *req)
{
	char *json = NULL;
	char value[1024];
	const char *base = "/api/packages";
	const char *path = req->url;

	log_debug("[PKG] Handling request: %s", req->url);

	if (!path || strncmp(path, base, strlen(base)) != 0) {
		return http_send_error(req, 400, "Bad Request");
	}

	path += strlen(base);

	if (strncmp(path, "/search", 7) == 0) {
		if (!get_query_value(req->url, "q", value, sizeof(value)))
			return http_send_error(req, 400, "Missing q parameter");
		log_debug("[PKG] Search query: %s", value);
		json = pkg_search_json(value);
	} else if (strncmp(path, "/info", 5) == 0) {
		if (!get_query_value(req->url, "name", value, sizeof(value)))
			return http_send_error(req, 400, "Missing name parameter");
		log_debug("[PKG] Info for: %s", value);
		json = pkg_info_json(value);
	} else if (strncmp(path, "/which", 6) == 0) {
		if (!get_query_value(req->url, "path", value, sizeof(value)))
			return http_send_error(req, 400, "Missing path parameter");
		log_debug("[PKG] Which for path: %s", value);
		json = pkg_which_json(value);
	} else if (strncmp(path, "/files", 6) == 0) {
		if (!get_query_value(req->url, "name", value, sizeof(value)))
			return http_send_error(req, 400, "Missing name parameter");
		log_debug("[PKG] Files for: %s", value);
		json = pkg_files_json(value);
	} else if (strncmp(path, "/list", 5) == 0) {
		log_debug("[PKG] Listing all packages");
		json = pkg_list_json();
	} else {
		return http_send_error(req, 404, "Unknown packages endpoint");
	}

	if (!json) {
		log_debug("[PKG] Failed to generate JSON");
		return http_send_error(req, 500, "Unable to query package manager");
	}

	log_debug("[PKG] Generated JSON (%zu bytes)", strlen(json));

	http_response_t *resp = http_response_create();
	resp->status_code = 200;
	resp->content_type = "application/json";
	http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
	http_response_set_body(resp, json, strlen(json), 1);

	int ret = http_response_send(req, resp);
	http_response_free(resp);

	log_debug("[PKG] Response sent, ret=%d", ret);
	return ret;
}

int
packages_module_attach_routes(struct router *r)
{
	// Registra il prefisso per tutti gli endpoint packages
	if (router_register_prefix(r, "GET", "/api/packages", 0, pkg_api_handler) != 0)
		return -1;

	// Registra anche i singoli endpoint per sicurezza
	if (router_register(r, "GET", "/api/packages/search", pkg_api_handler) != 0)
		return -1;
	if (router_register(r, "GET", "/api/packages/info", pkg_api_handler) != 0)
		return -1;
	if (router_register(r, "GET", "/api/packages/which", pkg_api_handler) != 0)
		return -1;
	if (router_register(r, "GET", "/api/packages/files", pkg_api_handler) != 0)
		return -1;
	if (router_register(r, "GET", "/api/packages/list", pkg_api_handler) != 0)
		return -1;

	return 0;
}
