/* man_module.c - man generation implementation */

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
#include <time.h>
#include <miniweb/router/router.h>

#include <miniweb/core/config.h>
#include <miniweb/http/handler.h>
#include <miniweb/http/utils.h>
#include <miniweb/modules/man.h>
#include <miniweb/router/routes.h>
#include <miniweb/core/log.h>

#define MAX_JSON_SIZE (256 * 1024)
#define MAX_OUTPUT_SIZE (10 * 1024 * 1024) //10 MB!
#define MAN_HOT_CACHE_TTL_SEC 30

static int is_valid_section(const char *section);

/**
 * @brief Return non-zero when an API endpoint matches exactly.
 * @param path Request URL path after /api/man.
 * @param endpoint Endpoint token such as "/sections".
 * @return 1 when @p path is @p endpoint, optionally followed by query string.
 */
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
 * @param src Encoded source string.
 * @param dst Destination buffer.
 * @param dst_size Destination buffer size.
 * @return 0 on success, -1 on invalid input or overflow.
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
 * @brief Extract and decode a query parameter value from an URL.
 * @param url Full request URL.
 * @param key Query key to extract.
 * @param out Destination buffer for decoded value.
 * @param out_size Destination buffer size.
 * @return 1 when found and decoded, 0 otherwise.
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

/**
 * @brief Parse a section suffix from a man page filename.
 * @param filename Leaf filename (for example "help.1.gz").
 * @param section_out Destination section string.
 * @param section_out_len Destination buffer size.
 * @return 1 on success, 0 when the filename does not include a valid section.
 */
static int
parse_section_from_filename(const char *filename, char *section_out,
			    size_t section_out_len)
{
	static const char *compressed_suffixes[] = {".gz", ".bz2", ".xz", ".zst"};
	char tmp[256];

	if (!filename || !section_out || section_out_len == 0)
		return 0;

	strlcpy(tmp, filename, sizeof(tmp));

	for (size_t i = 0; i < sizeof(compressed_suffixes) / sizeof(compressed_suffixes[0]); i++) {
		size_t tlen = strlen(tmp);
		size_t slen = strlen(compressed_suffixes[i]);
		if (tlen > slen && strcmp(tmp + tlen - slen, compressed_suffixes[i]) == 0) {
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

/* Remove nroff overstrike sequences (for example "N\bN", "_\bX")
 * from mandoc ASCII output so markdown fallback remains readable. */
/**
 * @brief Strip overstrike ascii.
 * @param text Parameter used by this function.
 * @param len Destination buffer length.
 */
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

/**
 * @brief Is valid token.
 * @param s Input string to parse or sanitize.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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

/**
 * @brief Is valid section.
 * @param section Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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
/**
 * @brief Resolve man path.
 * @param name Parameter used by this function.
 * @param section Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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
/**
 * @brief Man get sections json.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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

/**
 * @brief Man get section pages json.
 * @param area Parameter used by this function.
 * @param section Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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

	/*
	 * Build the JSON array using an explicit write offset so we always
	 * know how much space remains.  The previous strlcat-based loop was
	 * correct but silent: once the buffer filled, subsequent strlcat calls
	 * became no-ops and the closing "]}" was never written, producing
	 * truncated JSON that caused JSON.parse() to throw in the browser.
	 *
	 * Reserve 4 bytes at the end for the mandatory "]}" terminator plus a
	 * NUL and one byte of safety margin.
	 */
#define JSON_CLOSE_RESERVE 4

	int n = snprintf(json, MAX_JSON_SIZE, "{\"pages\":[");
	size_t used = (n > 0) ? (size_t)n : 0;

	struct dirent *de;
	int first = 1;

	while ((de = readdir(dr)) != NULL) {
		/* Skip hidden files and architecture subdirectories. */
		if (de->d_name[0] == '.')
			continue;
		if (de->d_type == DT_DIR)
			continue;

		/*
		 * Also follow symlinks — some man trees use .so-style symlinks
		 * for alias pages. DT_LNK is neither DT_DIR nor DT_REG but
		 * parse_section_from_filename works on the link name directly.
		 */

		/* Filter: filename must parse to the requested section. */
		char resolved_section[16];
		if (!parse_section_from_filename(de->d_name, resolved_section,
		    sizeof(resolved_section)))
			continue;
		if (strcmp(resolved_section, section) != 0)
			continue;

		/* Extract base name (everything before the first dot). */
		char name[128];
		char *dot = strchr(de->d_name, '.');
		size_t name_len = dot
		    ? (size_t)(dot - de->d_name)
		    : strlen(de->d_name);
		if (name_len >= sizeof(name))
			name_len = sizeof(name) - 1;
		memcpy(name, de->d_name, name_len);
		name[name_len] = '\0';

		/*
		 * Calculate how many bytes this entry needs:
		 *   separator ("," or "") + '"' + name + '"'
		 * plus the JSON_CLOSE_RESERVE we always keep.
		 */
		size_t entry_len = (first ? 0 : 1) + 1 + name_len + 1;
		if (used + entry_len + JSON_CLOSE_RESERVE >= MAX_JSON_SIZE)
			break; /* Graceful truncation — close array below. */

		n = snprintf(json + used,
		    MAX_JSON_SIZE - used - JSON_CLOSE_RESERVE,
		    "%s\"%s\"",
		    first ? "" : ",",
		    name);
		if (n > 0) {
			used += (size_t)n;
			first = 0;
		}
	}
	closedir(dr);

	/*
	 * Always close the JSON array.  Because we reserved JSON_CLOSE_RESERVE
	 * bytes above, this snprintf is guaranteed to succeed.
	 */
	(void)snprintf(json + used, MAX_JSON_SIZE - used, "]}");

#undef JSON_CLOSE_RESERVE

	return json;
}

/**
 * @brief Build metadata JSON for a resolved manual page.
 * @param area Manual area identifier.
 * @param section Manual section identifier.
 * @param name Manual page name.
 * @return Allocated JSON string with metadata or error object.
 */
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

/**
 * @brief Man api search.
 * @param query Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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
/**
 * @brief Area from path.
 * @param filepath Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
static const char *
area_from_path(const char *filepath)
{
	if (strncmp(filepath, "/usr/X11R6/", 11) == 0) return "x11";
	if (strncmp(filepath, "/usr/local/",  11) == 0) return "packages";
	return "system";
}

/**
 * @brief Mkdir p.
 * @param dir Parameter used by this function.
 * @param base_dir Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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

/**
 * @brief Write file binary.
 * @param path Request or filesystem path to evaluate.
 * @param buf Input buffer containing textual data.
 * @param len Destination buffer length.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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

/**
 * @brief Mime for format.
 * @param format Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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

/**
 * @brief Add content disposition header based on requested format.
 * @param resp Response object to mutate.
 * @param format Requested output format.
 * @param page Manual page name used for filename.
 */
static void
add_content_disposition_for_format(http_response_t *resp,
				   const char *format,
				   const char *page)
{
	char content_disp[256];

	if (strcmp(format, "pdf") == 0) {
		snprintf(content_disp, sizeof(content_disp), "inline; filename=\"%s.pdf\"", page);
		http_response_add_header(resp, "Content-Disposition", content_disp);
	} else if (strcmp(format, "md") == 0) {
		http_response_add_header(resp, "Content-Disposition", "inline");
	} else if (strcmp(format, "txt") == 0) {
		http_response_add_header(resp, "Content-Disposition", "inline");
	} else if (strcmp(format, "ps") == 0) {
		snprintf(content_disp, sizeof(content_disp), "attachment; filename=\"%s.ps\"", page);
		http_response_add_header(resp, "Content-Disposition", content_disp);
	}
}

/**
 * @brief Send a previously rendered manpage from cache storage.
 * @param req Request context.
 * @param cache_abs Absolute cache file path.
 * @param format Output format.
 * @param page Manual page name.
 * @return HTTP send result code or -1 on read/build failures.
 */
static int
is_hot_man_cache_hit(const char *cache_abs, const char *format)
{
	if (!cache_abs || !format)
		return 0;
	if (strcmp(format, "html") != 0 && strcmp(format, "md") != 0)
		return 0;

	struct stat st;
	if (stat(cache_abs, &st) != 0)
		return 0;

	time_t now = time(NULL);
	return (now - st.st_mtime) <= MAN_HOT_CACHE_TTL_SEC;
}

static int
is_static_cache_format(const char *format)
{
	return strcmp(format, "html") == 0 || strcmp(format, "txt") == 0 ||
		strcmp(format, "md") == 0 || strcmp(format, "ps") == 0 ||
		strcmp(format, "pdf") == 0;
}

/**
 * @brief Compute relative and absolute paths for manpage cache files.
 * @param area Manual area identifier.
 * @param section Manual section identifier.
 * @param page Manual page name.
 * @param format Output format extension.
 * @param rel Output buffer for the relative URL path.
 * @param rel_len Size of rel buffer.
 * @param abs Output buffer for the absolute filesystem path.
 * @param abs_len Size of abs buffer.
 * @return 0 on success, -1 if input is invalid or buffers are too small.
 */
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

/**
 * @brief Man api search raw.
 * @param query Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
char *
man_api_search_raw(const char *query)
{
	if (!query || strlen(query) < 2)
		return strdup("");

	/*
	 * Validate the query token before handing it to apropos.
	 * man_api_search() (JSON variant) already does this via is_valid_token.
	 * We mirror that check here so both paths behave consistently.
	 *
	 * is_valid_token rejects whitespace and shell-significant characters.
	 * Queries such as "ls -l" or "open files" contain spaces and are
	 * rejected; the FE search box should send single-keyword queries.
	 * If multi-word support is needed, widen the validator to accept spaces.
	 *
	 * execv (used by safe_popen_read_argv) is not a shell — there is no
	 * injection risk — but invalid tokens produce garbage apropos output
	 * that breaks the FE parseAproposLine parser.
	 */
	if (!is_valid_token(query))
		return strdup("");

	/* Use the full manual search path so results can include /usr/local/man
	 * after the local makewhatis database has been generated. */
	char *const argv[] = {
		"apropos", "-M",
		"/usr/share/man:/usr/local/man:/usr/X11R6/man",
		(char *)query, NULL
	};

	/* Increase buffer size because apropos may return a lot of text. */
	char *output = safe_popen_read_argv("/usr/bin/apropos", argv,
	    1024 * 1024, 5, NULL);

	if (!output)
		output = strdup("");

	/*
	 * Some installations do not have a populated whatis/apropos DB, which
	 * makes apropos return no lines even for valid pages.  Keep the endpoint
	 * useful by falling back to a direct page resolve and emitting one
	 * apropos-like line so the frontend parser still gets a non-empty payload.
	 */
	if (output[0] == '\0') {
		char *filepath = resolve_man_path(query, "1");
		if (!filepath)
			filepath = resolve_man_path(query, "8");
		if (!filepath) {
			char *const argv_w[] = {
				"man", "-M", "/usr/share/man:/usr/local/man:/usr/X11R6/man",
				"-w", (char *)query, NULL
			};
			filepath = safe_popen_read_argv("/usr/bin/man", argv_w, 1024, 5,
				NULL);
			if (filepath)
				filepath[strcspn(filepath, "\r\n")] = 0;
		}

		if (filepath && filepath[0] != '\0') {
			char section[16] = {0};
			const char *base = strrchr(filepath, '/');
			base = base ? base + 1 : filepath;
			if (!parse_section_from_filename(base, section, sizeof(section)))
				strlcpy(section, "?", sizeof(section));

			char line[256];
			snprintf(line, sizeof(line), "%s (%s) - manual page", query,
				 section);
			free(output);
			output = strdup(line);
		}
		free(filepath);
	}
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
	if (path_matches_endpoint(path, "/resolve")) {
		char name_buf[64]    = {0};
		char section_buf[16] = {0};

		/* Extract name= and section= from query string */
		(void)query_string;
		(void)get_query_value(req->url, "name", name_buf, sizeof(name_buf));
		(void)get_query_value(req->url, "section", section_buf,
				      sizeof(section_buf));

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
					if (!parse_section_from_filename(base,
								 resolved_section,
								 sizeof(resolved_section))) {
						strlcpy(resolved_section, section_buf,
							sizeof(resolved_section));
					}


					/* Escape filepath for JSON — paths from 'man -w'
					 * can contain spaces on unusual installations and
					 * can be up to PATH_MAX bytes long. A fixed 1024-
					 * byte stack buffer was too small; use a dynamically
					 * sized allocation instead. */
					char *escaped_path = json_escape_string(filepath);
					if (!escaped_path)
						escaped_path = strdup("");

					const char *sec_out =
					    resolved_section[0] ? resolved_section
					                        : section_buf;

					/* Calculate the exact required size, then allocate. */
					int needed = snprintf(NULL, 0,
					    "{\"name\":\"%s\","
					    "\"section\":\"%s\","
					    "\"area\":\"%s\","
					    "\"path\":\"%s\"}",
					    name_buf, sec_out, area,
					    escaped_path);
					if (needed > 0) {
						json = malloc((size_t)needed + 1);
						if (json) {
							snprintf(json, (size_t)needed + 1,
							    "{\"name\":\"%s\","
							    "\"section\":\"%s\","
							    "\"area\":\"%s\","
							    "\"path\":\"%s\"}",
							    name_buf, sec_out, area,
							    escaped_path);
						}
					}
					free(escaped_path);
					free(filepath);
				}
			}
	} else {
		/* Casi semplici: stringhe esatte senza parametri nel path */
		if (path_matches_endpoint(path, "/sections")) {
			json = man_get_sections_json();
		} else if (path_matches_endpoint(path, "/pages")) {
			/* Estrazione "section=" dalla query string */
			char section[16] = {0};
			char area[16] = "system";

			if (get_query_value(req->url, "section", section,
					    sizeof(section))) {
				(void)get_query_value(req->url, "area", area,
					      sizeof(area));
				json = man_get_section_pages_json(area, section);
			} else {
				json =
				strdup("{\"error\":\"Missing section parameter\"}");
			}
		} else if (strncmp(path, "/search/", 8) == 0 ||
			   path_matches_endpoint(path, "/search")) {
			const char *query = NULL;
			char query_buf[256] = {0};

			/* Check whether path is /api/man/search/open (JS format). */
			if (path[7] == '/') {
				query = path + 8; // Take everything after the slash
			}
			/* Check whether path is /api/man/search?q=open (curl format). */
			else {
				if (get_query_value(req->url, "q", query_buf,
						    sizeof(query_buf)))
					query = query_buf;
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
	char *filepath = resolve_man_path(page, section);
	if (!filepath) {
		char *const argv_w[] = {
			"man", "-M", "/usr/share/man:/usr/local/man:/usr/X11R6/man",
			"-w", (char *)page, NULL
		};
		filepath = safe_popen_read_argv("/usr/bin/man", argv_w, 1024, 5,
			NULL);
	}

	if (!filepath)
		return NULL;
	if (filepath) {
		/* Strip trailing newline */
		filepath[strcspn(filepath, "\r\n")] = 0;
	}

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

	/* Normalize mandoc ASCII output so text downloads/rendering are clean
	 * across browsers (no nroff overstrike backspace sequences). */
	if (output && *out_len > 0 && strcmp(format, "txt") == 0) {
		strip_overstrike_ascii(output, out_len);
	}

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
		log_debug("[MAN] PDF generated: size=%zu bytes", *out_len);
		log_debug("[MAN] PDF signature: %02x %02x %02x %02x",
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
		is_static_cache_format(format) && access(cache_abs, R_OK) == 0 &&
		is_hot_man_cache_hit(cache_abs, format)) {
		const char *old_url = req->url;
		req->url = cache_rel;
		int ret = static_handler(req);
		req->url = old_url;
		return ret;
	}

	/* 3. Render content. */
	size_t out_len = 0;
	char *output = man_render_page(area, section, page, format, &out_len);

	if (!output) {
		return http_send_error(req, 404, "Manual page not found");
	}

	char cache_dir[512];
	strlcpy(cache_dir, cache_abs, sizeof(cache_dir));
	char *last_slash = strrchr(cache_dir, '/');
	if (last_slash) {
		*last_slash = '\0';

		if (mkdir_p(cache_dir, config_static_dir) == 0) {
			if (write_file_binary(cache_abs, output, out_len) != 0 &&
				config_verbose) {
				log_debug(
					"[MAN] cache write failed: %s (errno=%d: %s)\n",
					cache_abs, errno, strerror(errno));
			} else if (config_verbose) {
				log_debug(
					"[MAN] cache write ok: %s (%zu bytes)\n",
					cache_abs, out_len);
			}
		} else if (config_verbose) {
			log_debug(
				"[MAN] cache directory create failed: %s (errno=%d: %s)\n",
				cache_dir, errno, strerror(errno));
		}
	}

	if (is_static_cache_format(format) && access(cache_abs, R_OK) == 0) {
		free(output);
		const char *old_url = req->url;
		req->url = cache_rel;
		int ret = static_handler(req);
		req->url = old_url;
		return ret;
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

	add_content_disposition_for_format(resp, format, page);

	/* 6. Set body (http_response_set_body frees 'output' automatically). */
	http_response_set_body(resp, output, out_len, 1);

	/* 7. Send response. */
	int ret = http_response_send(req, resp);
	http_response_free(resp);

	return ret;
}

int
man_module_attach_routes(struct router *r)
{
	if (router_register_prefix(r, "GET", "/man/", 2, man_render_handler) != 0)
		return -1;
	return router_register_prefix(r, "GET", "/api/man", 0, man_api_handler);
}
