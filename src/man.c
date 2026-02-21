#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/config.h"
#include "../include/http_handler.h"
#include "../include/http_utils.h"
#include "../include/man.h"
#include "../include/routes.h"

#define MAX_JSON_SIZE (256 * 1024)
#define MAX_OUTPUT_SIZE (10 * 1024 * 1024) //10 MB!

/* Remove nroff overstrike sequences (for example "N\bN", "_\bX")
 * from mandoc ASCII output so markdown fallback remains readable. */
static void
strip_overstrike_ascii(char *text, size_t *len)
{
	if (!text || !len)
		return;

	size_t in = 0;
	size_t out = 0;

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
		if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-' &&
			*p != '+')
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

/* Resolve the man page path using 'man -w'. */
static char *
resolve_man_path(const char *name, const char *section)
{
	if (!is_valid_token(name) || !is_valid_section(section))
		return NULL;

	char *const argv[] = {"man",
		"-M",
		"/usr/share/man:/usr/local/man:/usr/X11R6/man",
		"-w",
		(char *)section,
		(char *)name,
		NULL};
		char *path = safe_popen_read_argv("/usr/bin/man", argv, 512, 5, NULL);
		if (path) {
			path[strcspn(path, "\r\n")] = 0;
			if (strlen(path) == 0) {
				free(path);
				return NULL;
			}
		}
		return path;
}

/* --- API JSON --- */
char *
man_get_sections_json(void)
{
	return strdup("{"
	"\"system\":{"
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
	"]"
	"},"
	"\"x11\":{"
	"\"name\":\"X11 Window System\","
	"\"path\":\"/usr/X11R6/man\","
	"\"sections\":["
	"{\"id\":\"1\",\"name\":\"X11 Commands\"},"
	"{\"id\":\"3\",\"name\":\"X11 Library\"},"
	"{\"id\":\"4\",\"name\":\"X11 Drivers\"},"
	"{\"id\":\"5\",\"name\":\"X11 Formats\"},"
	"{\"id\":\"7\",\"name\":\"X11 Misc\"}"
	"]"
	"},"
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
	"]"
	"}"
	"}");
}

char *
man_get_section_pages_json(const char *area, const char *section)
{
	char dir_path[256];
	const char *base = "/usr/share/man";

	/* Select base path from the requested area. */
	if (strcmp(area, "packages") == 0) {
		base = "/usr/local/man";
	} else if (strcmp(area, "x11") == 0) {
		base = "/usr/X11R6/man";
	}

	/* Build section directory path, for example /usr/X11R6/man/man1. */
	snprintf(dir_path, sizeof(dir_path), "%s/man%s", base, section);

	DIR *dr = opendir(dir_path);
	if (!dr) {
		return strdup("{\"pages\":[]}");
	}

	char *json = malloc(MAX_JSON_SIZE);
	if (!json) {
		closedir(dr);
		return NULL;
	}

	/* Initialize JSON output. */
	strlcpy(json, "{\"pages\":[", MAX_JSON_SIZE);

	struct dirent *de;
	int first = 1;
	// size_t suffix_len = strlen(section) + 1; // Lunghezza di ".1", ".7",
	// ecc.

	while ((de = readdir(dr)) != NULL) {
		// Skip hidden files and architecture subdirectories (for example i386, alpha).
		if (de->d_name[0] == '.')
			continue;
		if (de->d_type == DT_DIR)
			continue;

		/* Filter: filename must end exactly with .<section>.
		 * Example: xterm.1 in man1 -> accepted.
		 * Example: i386 (directory) -> skipped by DT_DIR.
		 */
		char *dot = strrchr(de->d_name, '.');
		if (dot && strcmp(dot + 1, section) == 0) {
			char name[128];
			size_t name_len = dot - de->d_name;
			if (name_len >= sizeof(name))
				name_len = sizeof(name) - 1;

			memcpy(name, de->d_name, name_len);
			name[name_len] = '\0';

			if (!first)
				strlcat(json, ",", MAX_JSON_SIZE);
			strlcat(json, "\"", MAX_JSON_SIZE);
			strlcat(json, name, MAX_JSON_SIZE);
			strlcat(json, "\"", MAX_JSON_SIZE);
			first = 0;
		}
	}
	closedir(dr);
	strlcat(json, "]}", MAX_JSON_SIZE);

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
			 "{\"name\":\"%s\",\"section\":\"%s\",\"area\":\"%s\",\"path\":"
			 "\"%s\"}",
		  name, section, area, filepath);

	free(filepath);
	return json;
}

char *
man_api_search(const char *query)
{
	if (!is_valid_token(query))
		return strdup("");

	char *const argv[] = {"apropos", "-M",
		"/usr/share/man:/usr/local/man:/usr/X11R6/man",
		(char *)query, NULL};
		char *output = safe_popen_read_argv("/usr/bin/apropos", argv,
											MAX_OUTPUT_SIZE, 5, NULL);
		if (!output)
			return strdup("");

	return output;
}

/* util for search */
static const char *
area_from_path(const char *filepath)
{
	if (strncmp(filepath, "/usr/X11R6/", 11) == 0) return "x11";
	if (strncmp(filepath, "/usr/local/",  11) == 0) return "packages";
	return "system";
}

static int
mkdir_p(const char *dir, const char *base_dir)
{
	if (!dir || *dir == '\0')
		return -1;

	char tmp[512];
	strlcpy(tmp, dir, sizeof(tmp));

	/* Under unveil, attempting mkdir() on parent paths outside base_dir
	 * can return ENOENT even when those parents exist. Start recursive
	 * creation from the unveiled static_dir prefix. */
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

static int
build_cache_paths(const char *area, const char *section, const char *page,
				  const char *format, char *rel, size_t rel_len,
				  char *abs, size_t abs_len)
{
	if (!area || !section || !page || !format || !rel || !abs)
		return -1;

	int n = snprintf(rel, rel_len, "/static/man/%s/%s/%s.%s", area,
				 section, page, format);
	if (n < 0 || (size_t)n >= rel_len)
		return -1;

	n = snprintf(abs, abs_len, "%s/man/%s/%s/%s.%s", config_static_dir,
			 area, section, page, format);
	if (n < 0 || (size_t)n >= abs_len)
		return -1;

	return 0;
}

char *
man_api_search_raw(const char *query)
{
	if (!query || strlen(query) < 2)
		return strdup("");

	/* Use the full manual search path so results can include /usr/local/man
	 * after the local makewhatis database has been generated. */
	// char *const argv[] = {"apropos", (char *)query, NULL};
	char *const argv[] = {
		"apropos", "-M",
		"/usr/share/man:/usr/local/man:/usr/X11R6/man",
		(char *)query, NULL
	};

	/* Increase buffer size because apropos may return a lot of text. */
	char *output = safe_popen_read_argv("/usr/bin/apropos", argv,
										1024 * 1024, 5, NULL);

	if (!output)
		return strdup("");
	return output;
}

/* --- HTTP handlers (using the native http_send_* helpers) --- */
/**
 * Handle JSON API requests for manual pages.
 */
int
man_api_handler(http_request_t *req)
{
	char *json = NULL;

	/* 1. Find the command segment after /api/man */
	const char *api_base = "/api/man";
	const char *path = strstr(req->url, api_base);
	if (!path) {
		return http_send_error(req, 400, "Bad Request");
	}
	path += strlen(api_base);

	/* 2. Isolate query string from the path segment. */
	const char *query_string = strchr(path, '?');
	size_t path_len =
	query_string ? (size_t)(query_string - path) : strlen(path);

	/* --- Routing logic --- */

	/* /api/man/resolve?name=kqueue&section=2
	 * Resolves the real area+path of a man page via 'man -w' so the FE
	 * can build a correct /man/{area}/{section}/{name} link without guessing. */
	if (strncmp(path, "/resolve", 8) == 0) {
		char name_buf[64]    = {0};
		char section_buf[16] = {0};

		/* Extract name= and section= from query string */
		const char *qs = query_string ? query_string : strchr(path, '?');
		if (qs) {
			const char *p_name = strstr(qs, "name=");
			if (p_name) {
				p_name += 5;
				for (int i = 0; *p_name && *p_name != '&' && i < 63; i++)
					name_buf[i] = *p_name++;
			}
			const char *p_sec = strstr(qs, "section=");
			if (p_sec) {
				p_sec += 8;
				for (int i = 0; *p_sec && *p_sec != '&' && i < 15; i++)
					section_buf[i] = *p_sec++;
			}
		}

		if (name_buf[0] == '\0' || !is_valid_token(name_buf) ||
			(section_buf[0] != '\0' && !is_valid_section(section_buf))) {
			json = strdup("{\"error\":\"name parameter required\"}");
			} else {
				/* Use full MANPATH; optionally narrow by section */
				char *filepath = NULL;
				if (section_buf[0] != '\0') {
					char *const argv[] = {
						"man", "-M",
						"/usr/share/man:/usr/local/man:/usr/X11R6/man",
						"-w", section_buf, name_buf, NULL
					};
					filepath = safe_popen_read_argv("/usr/bin/man",
													argv, 512, 5, NULL);
				} else {
					char *const argv[] = {
						"man", "-M",
						"/usr/share/man:/usr/local/man:/usr/X11R6/man",
						"-w", name_buf, NULL
					};
					filepath = safe_popen_read_argv("/usr/bin/man",
													argv, 512, 5, NULL);
				}

				if (filepath) {
					filepath[strcspn(filepath, "\r\n")] = 0;
				}

				if (!filepath || filepath[0] == '\0') {
					free(filepath);
					json = strdup("{\"error\":\"not found\"}");
				} else {
					const char *area = area_from_path(filepath);

					/* Extract section from filename: ls.1 -> "1" */
					char resolved_section[16] = {0};
					const char *base = strrchr(filepath, '/');
					base = base ? base + 1 : filepath;
					const char *dot = strrchr(base, '.');
					if (dot && dot[1] != '\0'){
						strlcpy(resolved_section, dot + 1,
								sizeof(resolved_section));
					}


					/* Escape filepath for JSON (no untrusted input,
					 * but paths can contain spaces on weird installs) */
					char jbuf[1024];
					snprintf(jbuf, sizeof(jbuf),
							 "{\"name\":\"%s\","
							 "\"section\":\"%s\","
							 "\"area\":\"%s\","
							 "\"path\":\"%s\"}",
			  name_buf,
			  resolved_section[0] ? resolved_section
			  : section_buf,
			  area,
			  filepath);
					json = strdup(jbuf);
					free(filepath);
				}
			}
	} else
		/* Casi semplici: stringhe esatte senza parametri nel path */
		if (strncmp(path, "/sections", path_len) == 0 && path_len == 9) {
			json = man_get_sections_json();
		} else if (strncmp(path, "/pages", path_len) == 0) {
			/* Estrazione "section=" dalla query string */
			const char *q =
			query_string ? strstr(query_string, "section=") : NULL;
			if (q) {
				q += 8;
				char section[16] = {0};
				for (int i = 0; *q && *q != '&' && i < 15; i++)
					section[i++] = *q++;
				json = man_get_section_pages_json("system", section);
			} else {
				json =
				strdup("{\"error\":\"Missing section parameter\"}");
			}
		} else if (strncmp(path, "/search", 7) == 0) {
			const char *query = NULL;
			char query_buf[256] = {0};

			/* Check whether path is /api/man/search/open (JS format). */
			if (path[7] == '/') {
				query = path + 8; // Take everything after the slash
			}
			/* Check whether path is /api/man/search?q=open (curl format). */
			else {
				const char *q_param = strstr(path, "q=");
				if (q_param) {
					q_param += 2;
					int i = 0;
					while (*q_param && *q_param != '&' && i < 255) {
						query_buf[i++] = *q_param++;
					}
					query = query_buf;
				}
			}

			if (query && *query != '\0') {
				/* Important: call the function that returns plain text,
				 * not JSON. */
				json = man_api_search_raw(query);
			} else {
				json = strdup("");
			}
		}
		/* Dynamic endpoint case: /api/man/system/1 */
		else {
			char area[32] = {0};
			char section[16] = {0};
			/* Use a local path copy capped at path_len for sscanf. */
			char path_tmp[64] = {0};
			if (path_len < sizeof(path_tmp)) {
				strncpy(path_tmp, path, path_len);
				if (sscanf(path_tmp, "/%31[^/]/%15s", area, section) ==
					2) {
					json =
					man_get_section_pages_json(area, section);
					}
			}

			if (!json) {
				json = strdup("{\"error\":\"Unknown API endpoint or "
				"malformed path\"}");
			}
		}

		/* 3. Send the response */
		if (!json) {
			return http_send_error(req, 500, "Internal Server Error");
		}

		http_response_t *resp = http_response_create();
		http_response_set_status(resp, 200);
		/* For search responses, use text/plain for client-side JS handling. */
		if (strncmp(path, "/search", 7) == 0) {
			resp->content_type = "text/plain; charset=utf-8";
		} else {
			resp->content_type = "application/json";
		}
		http_response_add_header(resp, "Access-Control-Allow-Origin", "*");

		http_response_set_body(resp, json, strlen(json),
			   1); /* 1 = free json */

		int ret = http_response_send(req, resp);
		http_response_free(resp);

		return ret;
}

/**
 * Renders a man page via mandoc. Supports html, pdf, ps, md formats.
 * area parameter is accepted for API compat but ignored - full MANPATH
 * is always used; real area is derived from the resolved filepath.
 */
char *
man_render_page(const char *area, const char *section, const char *page,
				const char *format, size_t *out_len)
{
	/* area is accepted for API compatibility but we always use the full
	 * MANPATH so any page in any of the three trees is found regardless of
	 * what the caller passes. area_from_path() derives the real area from
	 * the resolved filepath when needed. */
	(void)area;

	/* 1. Resolve physical file path via 'man -w' with full MANPATH. */
	char *const argv_w[] = {
		"man", "-M", "/usr/share/man:/usr/local/man:/usr/X11R6/man",
		"-w", (char *)section, (char *)page, NULL
	};
	char *filepath =
	safe_popen_read_argv("/usr/bin/man", argv_w, 1024, 5, NULL);

	if (!filepath)
		return NULL;

	/* Strip trailing newline */
	filepath[strcspn(filepath, "\r\n")] = 0;

	/* Validate: man -w must return an absolute path.
	 * An empty string means the page was not found.
	 * A non-'/' first byte means something went wrong. */
	if (filepath[0] != '/') {
		free(filepath);
		return NULL;
	}

	/* 3. Select mandoc output format. */
	const char *t_arg = "html"; /* default */
	if (strcmp(format, "pdf") == 0)
		t_arg = "pdf";
	else if (strcmp(format, "ps") == 0)
		t_arg = "ps";
	else if (strcmp(format, "md") == 0)
		t_arg = "markdown";
	else if (strcmp(format, "txt") == 0)
		t_arg = "ascii";

	/* 4. Execute mandoc. Keep out_len as the authoritative output size. */
	/* For HTML output, pass -O style= so mandoc links our stylesheet
	 * instead of embedding its own minimal inline CSS.
	 * Other formats (pdf, ps, markdown) do not support -O style. */
	char *argv_m[10]; /* Sufficient argument buffer. */
	int argc = 0;

	argv_m[argc++] = "mandoc";
	argv_m[argc++] = "-T";
	argv_m[argc++] = (char *)t_arg;

	/* Add -O only for HTML output. */
	if (strcmp(t_arg, "html") == 0) {
		argv_m[argc++] = "-Ostyle=/static/css/custom.css";
	}

	argv_m[argc++] = filepath;
	argv_m[argc++] = NULL;

	char *output = safe_popen_read_argv("/usr/bin/mandoc", argv_m,
										MAX_OUTPUT_SIZE, 10, out_len);

	/* Fallback for man(7) pages that cannot be converted to markdown.
	 * Return plain ASCII text instead of surfacing a 404 for .md requests. */
	if (!output && strcmp(format, "md") == 0) {
		char *const argv_ascii[] = {
			"mandoc", "-T", "ascii", filepath, NULL
		};
		output = safe_popen_read_argv("/usr/bin/mandoc", argv_ascii,
							 MAX_OUTPUT_SIZE, 10, out_len);
		if (output && *out_len > 0) {
			strip_overstrike_ascii(output, out_len);
		}
	}

	/* Optional debug logging for PDF output verification. */
	if (config_verbose && strcmp(format, "pdf") == 0 && output && *out_len > 0) {
		fprintf(stderr, "[MAN] PDF generated: size=%zu bytes\n", *out_len);
		fprintf(stderr, "[MAN] PDF signature: %02x %02x %02x %02x\n",
				(unsigned char)output[0], (unsigned char)output[1],
				(unsigned char)output[2], (unsigned char)output[3]);
		/* A valid PDF starts with %PDF (25 50 44 46). */
	}

	/* Cleanup. */
	free(filepath);

	return output; /* output is raw binary, out_len contains the length
				 exact length */
}

/**
	* Handler for visual rendering of man pages.
	* Expected URL: /man/{area}/{section}/{page}[.format]
 */
int
man_render_handler(http_request_t *req)
{
	char area[32] = "system";
	char section[16] = "";
	char page[64] = "";
	char format[16] = "html";

	/* 1. Parse URL (example: /man/system/1/ls.html). */
	if (strncmp(req->url, "/man/", 5) != 0) {
		return http_send_error(req, 400, "Invalid URL");
	}

	const char *p = req->url + 5;
	char path_copy[256];
	strncpy(path_copy, p, sizeof(path_copy) - 1);
	path_copy[sizeof(path_copy) - 1] = '\0';

	char *token = strtok(path_copy, "/");
	if (token)
		strncpy(area, token, sizeof(area) - 1);

	token = strtok(NULL, "/");
	if (token)
		strncpy(section, token, sizeof(section) - 1);

	token = strtok(NULL, "/");
	if (token) {
		char *dot = strrchr(token, '.');
		if (dot) {
			*dot = '\0';
			strncpy(format, dot + 1, sizeof(format) - 1);
		}
		strncpy(page, token, sizeof(page) - 1);
	}

	/* 2. Minimal validation. */
	if (page[0] == '\0' || section[0] == '\0') {
		return http_send_error(req, 400,
							   "Missing section or page name");
	}
	if (strcmp(format, "html") != 0 && strcmp(format, "pdf") != 0 &&
		strcmp(format, "ps") != 0 && strcmp(format, "md") != 0 &&
		strcmp(format, "txt") != 0) {
		return http_send_error(req, 400, "Unsupported format");
	}

	char cache_rel[512];
	char cache_abs[512];
	if (build_cache_paths(area, section, page, format, cache_rel,
					  sizeof(cache_rel), cache_abs,
					  sizeof(cache_abs)) == 0 &&
		access(cache_abs, R_OK) == 0) {
		const char *original_url = req->url;
		req->url = cache_rel;
		int cached = static_handler(req);
		req->url = original_url;
		if (cached == 0)
			return 0;
	}

	/* 3. Render content. */
	size_t out_len = 0;
	char *output = man_render_page(area, section, page, format, &out_len);

	if (!output) {
		return http_send_error(req, 404, "Manual page not found");
	}

	/* 4. Build response. */
	http_response_t *resp = http_response_create();

	/* Set the appropriate Content-Type. */
	resp->content_type = mime_for_format(format);

	/* 5. Add Content-Length (essential for binary formats like PDF). */
	char clen[32];
	snprintf(clen, sizeof(clen), "%zu", out_len);
	http_response_add_header(resp, "Content-Length", clen);
	http_response_add_header(resp, "Cache-Control", "no-cache, no-store, must-revalidate");
	http_response_add_header(resp, "Pragma", "no-cache");
	http_response_add_header(resp, "Expires", "0");

	if (strcmp(format, "pdf") == 0) {
		char content_disp[256];
		snprintf(content_disp, sizeof(content_disp), "inline; filename=\"%s.pdf\"", page);
		http_response_add_header(resp, "Content-Disposition", content_disp);
	} else if (strcmp(format, "md") == 0) {
		char content_disp[256];
		snprintf(content_disp, sizeof(content_disp), "inline; filename=\"%s.md\"", page);
		http_response_add_header(resp, "Content-Disposition", content_disp);
	}

	char cache_dir[512];
	strlcpy(cache_dir, cache_abs, sizeof(cache_dir));
	char *last_slash = strrchr(cache_dir, '/');
	if (last_slash) {
		*last_slash = '\0';

		if (mkdir_p(cache_dir, config_static_dir) == 0) {
			if (write_file_binary(cache_abs, output, out_len) != 0 &&
				config_verbose) {
				fprintf(stderr,
					"[MAN] cache write failed: %s (errno=%d: %s)\n",
					cache_abs, errno, strerror(errno));
			} else if (config_verbose) {
				fprintf(stderr,
					"[MAN] cache write ok: %s (%zu bytes)\n",
					cache_abs, out_len);
			}
		} else if (config_verbose) {
			fprintf(stderr,
				"[MAN] cache directory create failed: %s (errno=%d: %s)\n",
				cache_dir, errno, strerror(errno));
		}
	}

	/* 6. Set body (http_response_set_body frees 'output' automatically). */
	http_response_set_body(resp, output, out_len, 1);

	/* 7. Send response. */
	int ret = http_response_send(req, resp);
	http_response_free(resp);

	return ret;
}
