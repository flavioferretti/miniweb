#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/http_handler.h"
#include "../include/http_utils.h"
#include "../include/pkg_manager.h"

#define PKG_JSON_MAX (1024 * 1024)
#define PKG_CMD_MAX_OUTPUT (8 * 1024 * 1024)

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
		const char *eq = strchr(entry, '=');
		const char *amp = strchr(entry, '&');

		if (!amp)
			amp = entry + strlen(entry);
		if (!eq || eq > amp) {
			qs = (*amp == '&') ? amp + 1 : amp;
			continue;
		}

		if ((size_t)(eq - entry) == key_len && strncmp(entry, key, key_len) == 0) {
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
	char *output;
	char *json;
	size_t offset = 0;
	int first = 1;

	if (!query || strlen(query) < 1)
		return strdup("{\"query\":\"\",\"packages\":[]}");

	output = NULL;
	if (is_safe_pkg_name(query)) {
		char *const argv[] = {"pkg_info", "-Q", (char *)query, NULL};
		output = safe_popen_read_argv("/usr/sbin/pkg_info", argv,
						     PKG_CMD_MAX_OUTPUT, 5, NULL);
	}

	json = malloc(PKG_JSON_MAX);
	if (!json) {
		free(output);
		return NULL;
	}

	char *query_escaped = json_escape_string(query);
	if (!query_escaped)
		query_escaped = strdup("");

	offset += snprintf(json + offset, PKG_JSON_MAX - offset,
			  "{\"query\":\"%s\",\"packages\":[", query_escaped);
	free(query_escaped);

	if (output) {
		char *saveptr = NULL;
		char *line = strtok_r(output, "\n", &saveptr);

		while (line && offset < PKG_JSON_MAX - 256) {
			line[strcspn(line, "\r")] = '\0';
			if (*line != '\0') {
				char *line_escaped = json_escape_string(line);
				offset += snprintf(json + offset, PKG_JSON_MAX - offset,
						   "%s\"%s\"", first ? "" : ",",
						   line_escaped ? line_escaped : "");
				free(line_escaped);
				first = 0;
			}
			line = strtok_r(NULL, "\n", &saveptr);
		}
	}

	offset += snprintf(json + offset, PKG_JSON_MAX - offset, "]}");
	free(output);

	return json;
}

char *
pkg_info_json(const char *package_name)
{
	if (!is_safe_pkg_name(package_name))
		return strdup("{\"error\":\"invalid package name\"}");

	char *const argv[] = {"pkg_info", (char *)package_name, NULL};
	char *output = safe_popen_read_argv("/usr/sbin/pkg_info", argv,
					    PKG_CMD_MAX_OUTPUT, 5, NULL);

	if (!output)
		return strdup("{\"error\":\"package not found\"}");

	char *escaped_name = json_escape_string(package_name);
	char *escaped_output = json_escape_string(output);
	free(output);

	if (!escaped_name || !escaped_output) {
		free(escaped_name);
		free(escaped_output);
		return NULL;
	}

	char *json = malloc(strlen(escaped_name) + strlen(escaped_output) + 64);
	if (!json) {
		free(escaped_name);
		free(escaped_output);
		return NULL;
	}

	snprintf(json, strlen(escaped_name) + strlen(escaped_output) + 64,
		 "{\"package\":\"%s\",\"info\":\"%s\"}",
		 escaped_name, escaped_output);

	free(escaped_name);
	free(escaped_output);
	return json;
}

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
	free(output);

	offset += snprintf(json + offset, PKG_JSON_MAX - offset, "]}");
	return json;
}

char *
pkg_which_json(const char *file_path)
{
	if (!file_path || file_path[0] != '/')
		return strdup("{\"error\":\"path must be absolute\"}");

	char *const argv[] = {"pkg_info", "-W", (char *)file_path, NULL};
	char *output = safe_popen_read_argv("/usr/sbin/pkg_info", argv,
					    PKG_CMD_MAX_OUTPUT, 5, NULL);

	char *escaped_path = json_escape_string(file_path);
	if (!escaped_path)
		escaped_path = strdup("");

	if (!output) {
		char *json = malloc(strlen(escaped_path) + 80);
		if (!json) {
			free(escaped_path);
			return NULL;
		}
		snprintf(json, strlen(escaped_path) + 80,
			 "{\"path\":\"%s\",\"packages\":[]}", escaped_path);
		free(escaped_path);
		return json;
	}

	char *json = malloc(PKG_JSON_MAX);
	if (!json) {
		free(escaped_path);
		free(output);
		return NULL;
	}

	size_t offset = 0;
	offset += snprintf(json + offset, PKG_JSON_MAX - offset,
			  "{\"path\":\"%s\",\"packages\":[", escaped_path);
	free(escaped_path);

	int first = 1;
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
	free(output);

	offset += snprintf(json + offset, PKG_JSON_MAX - offset, "]}");
	return json;
}

int
pkg_api_handler(http_request_t *req)
{
	char *json = NULL;
	char value[1024];
	const char *base = "/api/packages";
	const char *path = strstr(req->url, base);

	if (!path)
		return http_send_error(req, 400, "Bad Request");

	path += strlen(base);

	if (strncmp(path, "/search", 7) == 0) {
		if (!get_query_value(req->url, "q", value, sizeof(value)))
			return http_send_error(req, 400, "Missing q parameter");
		json = pkg_search_json(value);
	} else if (strncmp(path, "/info", 5) == 0) {
		if (!get_query_value(req->url, "name", value, sizeof(value)))
			return http_send_error(req, 400, "Missing name parameter");
		json = pkg_info_json(value);
	} else if (strncmp(path, "/which", 6) == 0) {
		if (!get_query_value(req->url, "path", value, sizeof(value)))
			return http_send_error(req, 400, "Missing path parameter");
		json = pkg_which_json(value);
	} else if (strncmp(path, "/files", 6) == 0) {
		if (!get_query_value(req->url, "name", value, sizeof(value)))
			return http_send_error(req, 400, "Missing name parameter");
		json = pkg_files_json(value);
	} else {
		return http_send_error(req, 404, "Unknown packages endpoint");
	}

	if (!json)
		return http_send_error(req, 500, "Unable to query package manager");

	http_response_t *resp = http_response_create();
	resp->status_code = 200;
	resp->content_type = "application/json";
	http_response_set_body(resp, json, strlen(json), 1);

	int ret = http_response_send(req, resp);
	http_response_free(resp);
	return ret;
}
