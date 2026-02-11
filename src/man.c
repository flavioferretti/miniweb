#include "../include/man.h"
#include "../include/http_utils.h"
#include <errno.h>
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define ALL_MAN_PATHS "/usr/share/man:/usr/local/man"
#define MAX_JSON_SIZE (256 * 1024)
#define MAX_OUTPUT_SIZE (1024 * 1024)

/* --- Core Logic --- */

char *
man_get_sections_json(void)
{
	return strdup("{"
		      "\"system\":{"
		      "\"name\":\"OpenBSD Base System\","
		      "\"path\":\"/usr/share/man\","
		      "\"sections\":["
		      "{\"id\":\"1\",\"name\":\"User Commands\"},"
		      "{\"id\":\"2\",\"name\":\"System Calls\"},"
		      "{\"id\":\"3\",\"name\":\"Library Functions\"},"
		      "{\"id\":\"3p\",\"name\":\"POSIX Library\"},"
		      "{\"id\":\"4\",\"name\":\"Device Drivers\"},"
		      "{\"id\":\"5\",\"name\":\"File Formats\"},"
		      "{\"id\":\"7\",\"name\":\"Miscellaneous\"},"
		      "{\"id\":\"8\",\"name\":\"System Administration\"}"
		      "]"
		      "},"
		      "\"packages\":{"
		      "\"name\":\"Installed Packages\","
		      "\"path\":\"/usr/local/man\","
		      "\"sections\":["
		      "{\"id\":\"1\",\"name\":\"User Commands\"},"
		      "{\"id\":\"3\",\"name\":\"Library Functions\"},"
		      "{\"id\":\"5\",\"name\":\"File Formats\"},"
		      "{\"id\":\"7\",\"name\":\"Miscellaneous\"},"
		      "{\"id\":\"8\",\"name\":\"System Administration\"}"
		      "]"
		      "}"
		      "}");
}

/* Verify that a man page exists in the specific section */
int
verify_man_page(const char *man_path, const char *section, const char *name)
{
	char cmd[512];
	/* Usa il nome originale senza sanitizzazione */
	snprintf(cmd, sizeof(cmd), "man -M %s -w -S %s %s 2>/dev/null",
		 man_path, section, name);

	char *result = safe_popen_read(cmd, 1024);
	if (!result)
		return 0;

	char pattern[32];
	snprintf(pattern, sizeof(pattern), "man%s/", section);

	int found = (strstr(result, pattern) != NULL);
	free(result);
	return found;
}

/* Determina se una man page è in system o packages */
const char *
determine_man_area(const char *name, const char *section)
{
	char cmd[512];
	char s_clean[16];

	strlcpy(s_clean, section, sizeof(s_clean));
	sanitize_string(s_clean);

	/* Prova prima in system (/usr/share/man) */
	snprintf(cmd, sizeof(cmd),
		 "man -M /usr/share/man -w -S %s %s 2>/dev/null", s_clean,
		 name);

	char *result = safe_popen_read(cmd, 1024);
	if (result) {
		free(result);
		return "system";
	}

	/* Prova in packages (/usr/local/man) */
	snprintf(cmd, sizeof(cmd),
		 "man -M /usr/local/man -w -S %s %s 2>/dev/null", s_clean,
		 name);

	result = safe_popen_read(cmd, 1024);
	if (result) {
		free(result);
		return "packages";
	}

	/* Se non trovato, default system (meglio di niente) */
	return "system";
}

char *
man_get_section_pages_json(const char *area, const char *section)
{
	if (!area || !section) {
		return strdup("{\"error\":\"Missing parameters\"}");
	}

	char s_clean[16];
	strlcpy(s_clean, section, sizeof(s_clean));
	sanitize_string(s_clean);

	const char *man_path = (strcmp(area, "packages") == 0)
				   ? "/usr/local/man"
				   : "/usr/share/man";

	/*
	 * OPENBSD: apropos con . elenca TUTTE le pagine della sezione
	 * L'output è nel formato: nome(sezione) - descrizione
	 */
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "apropos -M %s -s %s . 2>/dev/null",
		 man_path, s_clean);

	char *output = safe_popen_read(cmd, MAX_JSON_SIZE / 2);
	if (!output) {
		char *json = malloc(256);
		snprintf(json, 256,
			 "{\"area\":\"%s\",\"section\":\"%s\",\"pages\":[]}",
			 area, s_clean);
		return json;
	}

	char *json = malloc(MAX_JSON_SIZE);
	if (!json) {
		free(output);
		return strdup("{\"error\":\"Out of memory\"}");
	}

	size_t len = snprintf(json, MAX_JSON_SIZE,
			      "{\"area\":\"%s\",\"section\":\"%s\",\"pages\":[",
			      area, s_clean);

	/* Fai una copia dell'output perché strtok lo modifica */
	char *output_copy = strdup(output);
	char *line = strtok(output_copy, "\n");
	int first = 1;
	int count = 0;
	const int MAX_PAGES = 1000;

	while (line && count < MAX_PAGES && len < MAX_JSON_SIZE - 1024) {
		/* Salta linee vuote */
		if (strlen(line) == 0) {
			line = strtok(NULL, "\n");
			continue;
		}

		/* Cerca la prima parentesi aperta */
		char *open_paren = strchr(line, '(');
		if (!open_paren) {
			line = strtok(NULL, "\n");
			continue;
		}

		/* Il nome è tutto prima della parentesi */
		size_t name_len = open_paren - line;
		char *name = malloc(name_len + 1);
		memcpy(name, line, name_len);
		name[name_len] = '\0';

		/* Pulisci il nome (rimuovi spazi finali) */
		char *end = name + name_len - 1;
		while (end > name && (*end == ' ' || *end == '\t')) {
			*end = '\0';
			end--;
		}

		/* Cerca la descrizione dopo " - " */
		char *desc = strstr(line, " - ");
		if (desc) {
			desc += 3; /* Salta " - " */
		} else {
			desc = "No description available";
		}

		/* Pulisci la descrizione (rimuovi newline) */
		char *desc_clean = strdup(desc);
		desc_clean[strcspn(desc_clean, "\r\n")] = 0;

		if (strlen(name) > 0) {
			char *e_name = json_escape_string(name);
			char *e_desc = json_escape_string(desc_clean);

			int written =
			    snprintf(json + len, MAX_JSON_SIZE - len,
				     "%s{\"name\":\"%s\",\"desc\":\"%s\"}",
				     first ? "" : ",", e_name, e_desc);

			if (written > 0 &&
			    (size_t)written < MAX_JSON_SIZE - len) {
				len += written;
				first = 0;
				count++;
			}

			free(e_name);
			free(e_desc);
		}

		free(name);
		free(desc_clean);
		line = strtok(NULL, "\n");
	}

	snprintf(json + len, MAX_JSON_SIZE - len, "]}");

	free(output_copy);
	free(output);
	return json;
}

char *
man_render_page(const char *area, const char *section, const char *name,
		const char *format)
{
	if (!area || !section || !name || !format) {
		return NULL;
	}

	char s_clean[16];
	strlcpy(s_clean, section, sizeof(s_clean));
	sanitize_string(s_clean); // OK per la sezione

	/* NON sanitizzare il nome del comando! */
	const char *n_clean = name; // Usa il nome originale

	const char *man_path = "/usr/share/man";
	if (strcmp(area, "packages") == 0) {
		man_path = "/usr/local/man";
	} else if (strcmp(area, "system") != 0) {
		fprintf(stderr, "Invalid area: %s\n", area);
		return NULL;
	}

	/* Usa il nome originale per la verifica */
	if (!verify_man_page(man_path, s_clean, n_clean)) {
		fprintf(stderr, "Man page not found: %s(%s) in %s\n", n_clean,
			s_clean, area);
		return NULL;
	}

	const char *m_fmt = "html";
	if (strcmp(format, "md") == 0) {
		m_fmt = "markdown";
	} else if (strcmp(format, "pdf") == 0) {
		m_fmt = "pdf";
	} else if (strcmp(format, "html") != 0) {
		fprintf(stderr, "Invalid format: %s\n", format);
		return NULL;
	}

	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		 "timeout 30 man -M %s -S %s -T %s %s 2>/dev/null", man_path,
		 s_clean, m_fmt, n_clean); // n_clean NON sanitizzato!

	return safe_popen_read(cmd, MAX_OUTPUT_SIZE);
}

// char *
// man_render_page(const char *area, const char *section, const char *name,
// const char *format)
// {
// 	if (!area || !section || !name || !format) {
// 		return NULL;
// 	}
//
// 	char s_clean[16], n_clean[64];
// 	strlcpy(s_clean, section, sizeof(s_clean));
// 	strlcpy(n_clean, name, sizeof(n_clean));
// 	sanitize_string(s_clean);
// 	sanitize_string(n_clean);
//
// 	const char *man_path = "/usr/share/man";
// 	if (strcmp(area, "packages") == 0) {
// 		man_path = "/usr/local/man";
// 	} else if (strcmp(area, "system") != 0) {
// 		fprintf(stderr, "Invalid area: %s\n", area);
// 		return NULL;
// 	}
//
// 	if (!verify_man_page(man_path, s_clean, n_clean)) {
// 		fprintf(stderr, "Man page not found: %s(%s) in %s\n",
// 				n_clean, s_clean, area);
// 		return NULL;
// 	}
//
// 	const char *m_fmt = "html";
// 	if (strcmp(format, "md") == 0) {
// 		m_fmt = "markdown";
// 	} else if (strcmp(format, "pdf") == 0) {
// 		m_fmt = "pdf";
// 	} else if (strcmp(format, "html") != 0) {
// 		fprintf(stderr, "Invalid format: %s\n", format);
// 		return NULL;
// 	}
//
// 	char cmd[512];
// 	snprintf(cmd, sizeof(cmd),
// 			 "timeout 30 man -M %s -S %s -T %s %s 2>/dev/null",
// 		  man_path, s_clean, m_fmt, n_clean);
//
// 	return safe_popen_read(cmd, MAX_OUTPUT_SIZE);
// }

char *
man_get_page_metadata_json(const char *area, const char *section,
			   const char *name)
{
	if (!area || !section || !name) {
		return strdup("{\"error\":\"Missing parameters\"}");
	}

	char s_clean[16], a_clean[16];
	strlcpy(a_clean, area, sizeof(a_clean));
	strlcpy(s_clean, section, sizeof(s_clean));
	sanitize_string(a_clean);
	sanitize_string(s_clean);

	/* VERIFICA CHE LA PAGINA ESISTA DAVVERO IN QUEST'AREA */
	const char *man_path = (strcmp(area, "packages") == 0)
				   ? "/usr/local/man"
				   : "/usr/share/man";

	if (!verify_man_page(man_path, s_clean, name)) {
		char *json = malloc(256);
		snprintf(json, 256,
			 "{\"error\":\"Man page %s(%s) not found in %s\"}",
			 name, s_clean, area);
		return json;
	}

	char *json = malloc(2048);
	if (!json) {
		return strdup("{\"error\":\"Out of memory\"}");
	}

	char *e_name = json_escape_string(name);

	snprintf(json, 2048,
		 "{\"area\":\"%s\",\"section\":\"%s\",\"name\":\"%s\","
		 "\"formats\":[\"html\",\"md\",\"pdf\"],"
		 "\"links\":{"
		 "\"html\":\"/man/%s/%s/%s\","
		 "\"md\":\"/man/%s/%s/%s.md\","
		 "\"pdf\":\"/man/%s/%s/%s.pdf\""
		 "}}",
		 a_clean, s_clean, e_name, a_clean, s_clean, e_name, a_clean,
		 s_clean, e_name, a_clean, s_clean, e_name);

	free(e_name);
	return json;
}

// char *
// man_get_page_metadata_json(const char *area, const char *section, const char
// *name)
// {
// 	if (!area || !section || !name) {
// 		return strdup("{\"error\":\"Missing parameters\"}");
// 	}
//
// 	char s_clean[16], n_clean[64], a_clean[16];
// 	strlcpy(a_clean, area, sizeof(a_clean));
// 	strlcpy(s_clean, section, sizeof(s_clean));
// 	strlcpy(n_clean, name, sizeof(n_clean));
// 	sanitize_string(a_clean);
// 	sanitize_string(s_clean);
// 	sanitize_string(n_clean);
//
// 	char *json = malloc(2048);
// 	if (!json) {
// 		return strdup("{\"error\":\"Out of memory\"}");
// 	}
//
// 	snprintf(json, 2048,
// 			 "{\"area\":\"%s\",\"section\":\"%s\",\"name\":\"%s\",\"formats\":[\"html\",\"md\",\"pdf\"],"
// 			 "\"links\":{"
// 			 "\"html\":\"/man/%s/%s/%s\","
// 			 "\"md\":\"/man/%s/%s/%s.md\","
// 			 "\"pdf\":\"/man/%s/%s/%s.pdf\""
// 			 "}}",
// 		  a_clean, s_clean, n_clean,
// 		  a_clean, s_clean, n_clean,
// 		  a_clean, s_clean, n_clean,
// 		  a_clean, s_clean, n_clean);
//
// 	return json;
// }

char *
man_search_json(const char *query)
{
	if (!query || strlen(query) == 0) {
		return strdup("{\"error\":\"Empty query\",\"results\":[]}");
	}

	/* OpenBSD: whatis NON restituisce il path, quindi dobbiamo verificare
	 * noi */
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "whatis -M %s %s 2>/dev/null | head -n 50",
		 ALL_MAN_PATHS, query);

	char *output = safe_popen_read(cmd, MAX_JSON_SIZE / 2);

	if (!output) {
		/* Fallback: apropos con match esatto */
		snprintf(cmd, sizeof(cmd),
			 "apropos -M %s '^%s$' 2>/dev/null | head -n 50",
			 ALL_MAN_PATHS, query);
		output = safe_popen_read(cmd, MAX_JSON_SIZE / 2);

		if (!output) {
			char *json = malloc(256);
			snprintf(json, 256, "{\"query\":\"%s\",\"results\":[]}",
				 query);
			return json;
		}
	}

	char *json = malloc(MAX_JSON_SIZE);
	if (!json) {
		free(output);
		return strdup("{\"error\":\"Out of memory\",\"results\":[]}");
	}

	size_t len = snprintf(json, MAX_JSON_SIZE,
			      "{\"query\":\"%s\",\"results\":[", query);

	char *line = strtok(output, "\n");
	int first = 1;
	int count = 0;
	const int MAX_RESULTS = 50;

	while (line && count < MAX_RESULTS && len < MAX_JSON_SIZE - 1024) {
		if (strlen(line) == 0) {
			line = strtok(NULL, "\n");
			continue;
		}

		/* Parsing: comando(sezione) - descrizione */
		char *open_paren = strchr(line, '(');
		if (!open_paren) {
			line = strtok(NULL, "\n");
			continue;
		}

		/* Nome = tutto prima della parentesi */
		size_t name_len = open_paren - line;
		char *name = malloc(name_len + 1);
		memcpy(name, line, name_len);
		name[name_len] = '\0';

		/* Pulisci nome (rimuovi spazi finali) */
		char *end = name + name_len - 1;
		while (end > name &&
		       (*end == ' ' || *end == '\t' || *end == '\r')) {
			*end = '\0';
			end--;
		}

		/* Sezione = dentro le parentesi */
		char *close_paren = strchr(open_paren, ')');
		if (!close_paren) {
			free(name);
			line = strtok(NULL, "\n");
			continue;
		}

		size_t sec_len = close_paren - open_paren - 1;
		char *section = malloc(sec_len + 1);
		memcpy(section, open_paren + 1, sec_len);
		section[sec_len] = '\0';

		/* Pulisci sezione (rimuovi spazi) */
		end = section + sec_len - 1;
		while (end > section &&
		       (*end == ' ' || *end == '\t' || *end == '\r')) {
			*end = '\0';
			end--;
		}

		/* Descrizione = dopo " - " */
		char *desc = strstr(line, " - ");
		if (desc) {
			desc += 3;
		} else {
			desc = "No description available";
		}

		char *desc_clean = strdup(desc);
		desc_clean[strcspn(desc_clean, "\r\n")] = 0;

		/* VERIFICA REALE DELL'AREA - questo è fondamentale! */
		const char *area = determine_man_area(name, section);

		if (!first) {
			len += snprintf(json + len, MAX_JSON_SIZE - len, ",");
		}

		char *e_name = json_escape_string(name);
		char *e_desc = json_escape_string(desc_clean);
		char *e_sec = json_escape_string(section);

		int written = snprintf(json + len, MAX_JSON_SIZE - len,
				       "{\"name\":\"%s\",\"section\":\"%s\","
				       "\"desc\":\"%s\",\"area\":\"%s\"}",
				       e_name, e_sec, e_desc, area);

		if (written > 0 && (size_t)written < MAX_JSON_SIZE - len) {
			len += written;
			first = 0;
			count++;
		}

		free(e_name);
		free(e_desc);
		free(e_sec);
		free(name);
		free(section);
		free(desc_clean);
		line = strtok(NULL, "\n");
	}

	snprintf(json + len, MAX_JSON_SIZE - len, "]}");

	free(output);
	return json;
}

/* --- HTTP Handlers --- */

int
man_render_handler(void *cls, struct MHD_Connection *connection,
		   const char *url, const char *method, const char *version,
		   const char *upload_data, size_t *upload_data_size,
		   void **con_cls)
{
	(void)cls;
	(void)method;
	(void)version;
	(void)upload_data;
	(void)upload_data_size;
	(void)con_cls;

	const char *path = url + strlen("/man/");
	char area[16] = {0}, section[16] = {0}, name[64] = {0},
	     format[16] = "html";
	const char *c_type = "text/html; charset=utf-8";

	const char *slash1 = strchr(path, '/');
	if (!slash1) {
		return http_queue_400(
		    connection,
		    "Invalid URL format. Expected: /man/area/section/name");
	}

	size_t area_len = slash1 - path;
	if (area_len >= sizeof(area))
		return MHD_NO;
	memcpy(area, path, area_len);
	area[area_len] = '\0';

	if (strcmp(area, "system") != 0 && strcmp(area, "packages") != 0) {
		return http_queue_400(
		    connection, "Invalid area. Use 'system' or 'packages'");
	}

	const char *rest = slash1 + 1;
	const char *slash2 = strchr(rest, '/');
	if (!slash2) {
		return http_queue_400(connection, "Missing section");
	}

	size_t sec_len = slash2 - rest;
	if (sec_len >= sizeof(section))
		sec_len = sizeof(section) - 1;
	memcpy(section, rest, sec_len);
	section[sec_len] = '\0';

	const char *name_ptr = slash2 + 1;
	const char *dot = strrchr(name_ptr, '.');

	if (dot) {
		size_t name_len = dot - name_ptr;
		if (name_len >= sizeof(name))
			name_len = sizeof(name) - 1;
		memcpy(name, name_ptr, name_len);
		name[name_len] = '\0';

		/* NON sanitizzare il nome qui! */

		if (strcmp(dot, ".md") == 0) {
			strlcpy(format, "md", sizeof(format));
			c_type = "text/markdown; charset=utf-8";
		} else if (strcmp(dot, ".pdf") == 0) {
			strlcpy(format, "pdf", sizeof(format));
			c_type = "application/pdf";
		}
	} else {
		strlcpy(name, name_ptr, sizeof(name));
		/* NON sanitizzare! */
	}

	char *output = man_render_page(area, section, name, format);
	if (!output) {
		return http_queue_404(connection, url);
	}

	struct MHD_Response *res = MHD_create_response_from_buffer(
	    strlen(output), output, MHD_RESPMEM_MUST_FREE);
	MHD_add_response_header(res, "Content-Type", c_type);
	MHD_add_response_header(res, "Cache-Control", "public, max-age=3600");

	int ret = MHD_queue_response(connection, MHD_HTTP_OK, res);
	MHD_destroy_response(res);
	return ret;
}

int
man_api_handler(void *cls, struct MHD_Connection *connection, const char *url,
		const char *method, const char *version,
		const char *upload_data, size_t *upload_data_size,
		void **con_cls)
{
	(void)cls;
	(void)method;
	(void)version;
	(void)upload_data;
	(void)upload_data_size;
	(void)con_cls;

	char *json = NULL;
	const char *path = url + strlen("/api/man");

	/* /api/man/search?q=query */
	if (strncmp(path, "/search", 7) == 0) {
		const char *q = MHD_lookup_connection_value(
		    connection, MHD_GET_ARGUMENT_KIND, "q");
		if (q && strlen(q) > 0) {
			json = man_search_json(q);
		} else {
			json = strdup("{\"results\":[],\"error\":\"Missing or "
				      "empty query parameter\"}");
		}
	}
	/* /api/man/ */
	else if (*path == '\0' || strcmp(path, "/") == 0) {
		json = man_get_sections_json();
	}
	/* /api/man/area/section or /api/man/area/section/name */
	else {
		/* Skip leading slash */
		const char *rest = (*path == '/') ? path + 1 : path;

		/* Parse area - everything before first slash */
		const char *slash1 = strchr(rest, '/');
		if (!slash1) {
			json = strdup("{\"error\":\"Invalid path. Use "
				      "/api/man/area/section\"}");
		} else {
			char area[16] = {0};
			size_t area_len = slash1 - rest;
			if (area_len >= sizeof(area))
				area_len = sizeof(area) - 1;
			memcpy(area, rest, area_len);
			area[area_len] = '\0';

			/* Validate area */
			if (strcmp(area, "system") != 0 &&
			    strcmp(area, "packages") != 0) {
				json = strdup("{\"error\":\"Invalid area. Use "
					      "'system' or 'packages'\"}");
			} else {
				const char *remaining = slash1 + 1;
				const char *slash2 = strchr(remaining, '/');

				if (!slash2) {
					/* /api/man/area/section - list all
					 * pages in section */
					json = man_get_section_pages_json(
					    area, remaining);
				} else {
					/* /api/man/area/section/name - get page
					 * metadata */
					char section[16] = {0};
					size_t sec_len = slash2 - remaining;
					if (sec_len >= sizeof(section))
						sec_len = sizeof(section) - 1;
					memcpy(section, remaining, sec_len);
					section[sec_len] = '\0';

					const char *name = slash2 + 1;
					json = man_get_page_metadata_json(
					    area, section, name);
				}
			}
		}
	}

	if (!json) {
		json = strdup("{\"error\":\"Internal server error\"}");
	}

	struct MHD_Response *res = MHD_create_response_from_buffer(
	    strlen(json), json, MHD_RESPMEM_MUST_FREE);
	MHD_add_response_header(res, "Content-Type",
				"application/json; charset=utf-8");
	MHD_add_response_header(res, "Access-Control-Allow-Origin", "*");

	int ret = MHD_queue_response(connection, MHD_HTTP_OK, res);
	MHD_destroy_response(res);
	return ret;
}
