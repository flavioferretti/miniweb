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

#define MAX_JSON_SIZE (256 * 1024)
#define MAX_OUTPUT_SIZE (10 * 1024 * 1024) //10 MB!

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

/* Risolve il path della pagina man usando 'man -w' */
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

	// Selezione del path base in base all'area
	if (strcmp(area, "packages") == 0) {
		base = "/usr/local/man";
	} else if (strcmp(area, "x11") == 0) {
		base = "/usr/X11R6/man";
	}

	// Costruisce il path della cartella, es: /usr/X11R6/man/man1
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

	// Inizializzazione JSON
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

		/* * FILTRO: Il file deve terminare esattamente con .<sezione>
		 * Esempio: xterm.1 in man1 -> OK.
		 * Esempio: i386 (directory) -> Scartato da DT_DIR.
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
				/* IMPORTANTE: qui chiamiamo la funzione che restituisce
				 * TESTO, non JSON */
				json = man_api_search_raw(query);
			} else {
				json = strdup("");
			}
		}
		/* Caso dinamico: /api/man/system/1 */
		else {
			char area[32] = {0};
			char section[16] = {0};
			/* Usiamo una copia locale del path limitata a path_len per
			 * sscanf */
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

		/* 3. Invio della risposta */
		if (!json) {
			return http_send_error(req, 500, "Internal Server Error");
		}

		http_response_t *resp = http_response_create();
		http_response_set_status(resp, 200);
		/* Se stiamo rispondendo a una ricerca, usiamo text/plain per il JS */
		if (strncmp(path, "/search", 7) == 0) {
			resp->content_type = "text/plain; charset=utf-8";
		} else {
			resp->content_type = "application/json";
		}
		http_response_add_header(resp, "Access-Control-Allow-Origin", "*");

		http_response_set_body(resp, json, strlen(json),
							   1); /* 1 = libera json */

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

	/* 1. Resolve physical file path via 'man -w' with full MANPATH */
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

	/* 3. Scelta del formato per mandoc */
	const char *t_arg = "html"; // default
	if (strcmp(format, "pdf") == 0)
		t_arg = "pdf";
	else if (strcmp(format, "ps") == 0)
		t_arg = "ps";
	else if (strcmp(format, "md") == 0)
		t_arg = "markdown";

	/* 4. Esecuzione di mandoc - IMPORTANTE: usiamo out_len per ottenere la
	 * lunghezza reale */
	/* For HTML output, pass -O style= so mandoc links our stylesheet
	 * instead of embedding its own minimal inline CSS.
	 * Other formats (pdf, ps, markdown) do not support -O style. */
	/* 4. Esecuzione di mandoc */
	char *argv_m[10]; // Buffer sufficiente per gli argomenti
	int argc = 0;

	argv_m[argc++] = "mandoc";
	argv_m[argc++] = "-T";
	argv_m[argc++] = (char *)t_arg;

	/* Aggiungiamo -O solo per l'HTML */
	if (strcmp(t_arg, "html") == 0) {
		argv_m[argc++] = "-Ostyle=/static/css/custom.css";
	}

	argv_m[argc++] = filepath;
	argv_m[argc++] = NULL;

	char *output = safe_popen_read_argv("/usr/bin/mandoc", argv_m,
										MAX_OUTPUT_SIZE, 10, out_len);

	/* Aggiungi questo debug temporaneo per verificare l'output */
	if (config_verbose && strcmp(format, "pdf") == 0 && output && *out_len > 0) {
		fprintf(stderr, "[MAN] PDF generated: size=%zu bytes\n", *out_len);
		fprintf(stderr, "[MAN] PDF signature: %02x %02x %02x %02x\n",
				(unsigned char)output[0], (unsigned char)output[1],
				(unsigned char)output[2], (unsigned char)output[3]);
		/* Un PDF valido inizia con %PDF (25 50 44 46) */
	}

	/* Pulizia */
	free(filepath);

	return output; /* output is raw binary, out_len contains the length
	esatta */
}

/**
 * Handler per il rendering visuale delle pagine man.
 * URL previsto: /man/{area}/{section}/{page}[.format]
 */
int
man_render_handler(http_request_t *req)
{
	char area[32] = "system";
	char section[16] = "";
	char page[64] = "";
	char format[16] = "html";

	/* 1. Parsing dell'URL (es: /man/system/1/ls.html) */
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

	/* 2. Validazione minima */
	if (page[0] == '\0' || section[0] == '\0') {
		return http_send_error(req, 400,
							   "Missing section or page name");
	}

	/* 3. Rendering */
	size_t out_len = 0;
	char *output = man_render_page(area, section, page, format, &out_len);

	if (!output) {
		return http_send_error(req, 404, "Manual page not found");
	}

	/* 4. Preparazione Risposta */
	http_response_t *resp = http_response_create();

	// Imposta il Content-Type corretto
	if (strcmp(format, "pdf") == 0) {
		resp->content_type = "application/pdf";
	} else if (strcmp(format, "ps") == 0) {
		resp->content_type = "application/postscript";
	} else if (strcmp(format, "md") == 0) {
		resp->content_type = "text/markdown; charset=utf-8";
	} else {
		resp->content_type = "text/html; charset=utf-8";
	}

	/* 5. Aggiunta Content-Length (fondamentale per file binari/PDF) */
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
	}

	/* 6. Impostazione del corpo (http_response_set_body libera 'output'
	 * automaticamente) */
	http_response_set_body(resp, output, out_len, 1);

	/* 7. Invio */
	int ret = http_response_send(req, resp);
	http_response_free(resp);

	return ret;
}
