#include "../include/man.h"
#include <ctype.h>
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALL_MAN_PATHS "/usr/share/man:/usr/local/man"

/* --- Utility Functions --- */

static void
sanitize(char *s)
{
	while (*s) {
		if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-')
			*s = '_';
		s++;
	}
}

static char *
read_command_output(const char *cmd, size_t max_size)
{
	FILE *fp = popen(cmd, "r");
	if (!fp)
		return NULL;

	size_t capacity = 16384;
	size_t total = 0;
	char *buffer = malloc(capacity);
	if (!buffer) {
		pclose(fp);
		return NULL;
	}

	char chunk[4096];
	size_t n;
	while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
		if (total + n + 1 > capacity) {
			if (capacity >= max_size)
				break;
			capacity *= 2;
			char *new_buf = realloc(buffer, capacity);
			if (!new_buf)
				break;
			buffer = new_buf;
		}
		memcpy(buffer + total, chunk, n);
		total += n;
	}
	buffer[total] = '\0';
	pclose(fp);
	return buffer;
}

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

/* Funzione per l'escaping dei caratteri JSON (fondamentale!) */
/* Funzione di escape migliorata */
static void
json_escape(char *dest, const char *src, size_t max) {
	size_t d = 0;
	for (size_t s = 0; src[s] != '\0' && d < max - 5; s++) {
		switch (src[s]) {
			case '"':  dest[d++] = '\\'; dest[d++] = '"';  break;
			case '\\': dest[d++] = '\\'; dest[d++] = '\\'; break;
			case '\b': dest[d++] = '\\'; dest[d++] = 'b';  break;
			case '\f': dest[d++] = '\\'; dest[d++] = 'f';  break;
			case '\n': dest[d++] = '\\'; dest[d++] = 'n';  break;
			case '\r': dest[d++] = '\\'; dest[d++] = 'r';  break;
			case '\t': dest[d++] = '\\'; dest[d++] = 't';  break;
			default:   dest[d++] = src[s]; break;
		}
	}
	dest[d] = '\0';
}

char *
man_get_section_pages_json(const char *area, const char *section)
{
	char s_clean[16];
	strlcpy(s_clean, section, sizeof(s_clean));
	sanitize(s_clean);

	/* Determina il path base in base all'area */
	const char *man_path = "/usr/share/man";
	if (area && strcmp(area, "packages") == 0) {
		man_path = "/usr/local/man";
	}

	char cmd[512];
	/* * Su OpenBSD mandoc, -s limita la sezione.
	 * Usiamo '^' per listare tutto ciò che inizia con un carattere alfanumerico
	 * limitatamente alla sezione specificata.
	 */
	snprintf(cmd, sizeof(cmd), "apropos -M %s -s %s ^", man_path, s_clean);

	FILE *fp = popen(cmd, "r");
	if (!fp) return NULL;

	char *json = malloc(MAX_JSON_SIZE);
	if (!json) { pclose(fp); return NULL; }

	/* Inizializzazione JSON pulita */
	size_t len = snprintf(json, MAX_JSON_SIZE,
						  "{\"area\":\"%s\",\"section\":\"%s\",\"pages\":[",
					   area ? area : "system", s_clean);
	char line[1024];
	int first = 1;

	while (fgets(line, sizeof(line), fp)) {
		// Controllo di sicurezza per non sforare il buffer (lasciamo spazio per chiusura)
		if (len > MAX_JSON_SIZE - 1024) break;

		char *name = strtok(line, "(");
		char *rest = strtok(NULL, "");
		if (!name || !rest) continue;

		/* Cerchiamo la descrizione dopo il separatore " - " */
		char *desc = strstr(rest, " - ");
		if (desc) {
			desc += 3; // Salta " - "
		} else {
			desc = "No description available";
		}

		/* Pulizia stringhe */
		name[strcspn(name, " \t\n\r")] = 0;
		desc[strcspn(desc, "\r\n")] = 0;

		/* Escaping dei caratteri che rompono il JSON */
		char e_name[128], e_desc[512];
		json_escape(e_name, name, sizeof(e_name));
		json_escape(e_desc, desc, sizeof(e_desc));

		int written = snprintf(json + len, MAX_JSON_SIZE - len,
							   "%s{\"name\":\"%s\",\"desc\":\"%s\"}",
						 first ? "" : ",", e_name, e_desc);

		if (written > 0) len += written;
		first = 0;
	}

	snprintf(json + len, MAX_JSON_SIZE - len, "]}");
	pclose(fp);
	return json;
}

char *
man_render_page(const char *area, const char *section, const char *name, const char *format)
{
	char s_clean[16], n_clean[64];
	strlcpy(s_clean, section, sizeof(s_clean));
	strlcpy(n_clean, name, sizeof(n_clean));
	sanitize(s_clean);
	sanitize(n_clean);

	/* Determina il path base in base all'area */
	const char *man_path = "/usr/share/man";
	if (area && strcmp(area, "packages") == 0) {
		man_path = "/usr/local/man";
	}

	const char *m_fmt = "html";
	if (strcmp(format, "md") == 0) m_fmt = "markdown";
	else if (strcmp(format, "pdf") == 0) m_fmt = "pdf";

	char cmd[512];
	/* Delega a man(1) la risoluzione dei percorsi */
	snprintf(cmd, sizeof(cmd), "man -M %s -S %s -T %s %s 2>/dev/null",
			 man_path, s_clean, m_fmt, n_clean);

	return read_command_output(cmd, MAX_OUTPUT_SIZE);
}

/* --- HTTP Handlers --- */

int
man_render_handler(void *cls, struct MHD_Connection *connection,
				   const char *url, const char *method, const char *version,
				   const char *upload_data, size_t *upload_data_size,
				   void **con_cls)
{
	(void)cls; (void)method; (void)version; (void)upload_data; (void)upload_data_size; (void)con_cls;

	/* Parse URL: /man/area/section/name[.format] */
	const char *path = url + strlen("/man/");

	char area[16] = {0}, section[16] = {0}, name[64] = {0}, format[16] = "html";
	const char *c_type = "text/html; charset=utf-8";

	/* Extract area */
	const char *slash1 = strchr(path, '/');
	if (!slash1) return MHD_NO;

	size_t area_len = slash1 - path;
	if (area_len >= sizeof(area)) return MHD_NO;
	memcpy(area, path, area_len);

	/* Validate area */
	if (strcmp(area, "system") != 0 && strcmp(area, "packages") != 0) return MHD_NO;

	/* Extract section */
	const char *rest = slash1 + 1;
	const char *slash2 = strchr(rest, '/');
	if (!slash2) return MHD_NO;

	size_t sec_len = slash2 - rest;
	if (sec_len >= sizeof(section)) sec_len = sizeof(section) - 1;
	memcpy(section, rest, sec_len);

	/* Extract name and format */
	const char *name_ptr = slash2 + 1;
	const char *dot = strrchr(name_ptr, '.');

	if (dot) {
		size_t name_len = dot - name_ptr;
		if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
		memcpy(name, name_ptr, name_len);

		if (strcmp(dot, ".md") == 0) {
			strlcpy(format, "md", sizeof(format));
			c_type = "text/plain; charset=utf-8"; // da text/markdown (no browser) a text/plain
		} else if (strcmp(dot, ".pdf") == 0) {
			strlcpy(format, "pdf", sizeof(format));
			c_type = "application/pdf";
		}
	} else {
		strlcpy(name, name_ptr, sizeof(name));
	}

	char *output = man_render_page(area, section, name, format);
	if (!output) return MHD_NO;

	struct MHD_Response *res = MHD_create_response_from_buffer(
		strlen(output), output, MHD_RESPMEM_MUST_FREE);
	MHD_add_response_header(res, "Content-Type", c_type);
	int ret = MHD_queue_response(connection, MHD_HTTP_OK, res);
	MHD_destroy_response(res);
	return ret;
}

/* --- HTTP Handlers per libmicrohttpd --- */

int
man_api_handler(void *cls, struct MHD_Connection *connection,
				const char *url, const char *method, const char *version,
				const char *upload_data, size_t *upload_data_size,
				void **con_cls)
{
	(void)cls; (void)method; (void)version; (void)upload_data; (void)upload_data_size; (void)con_cls;

	char *json = NULL;
	const char *path = url + strlen("/api/man");

	/* Routing interno all'API */
	if (strncmp(path, "/search", 7) == 0) {
		const char *q = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "q");
		if (q) {
			json = man_search_json(q);
		} else {
			json = strdup("{\"results\":[]}");
		}
	} else if (*path == '\0' || strcmp(path, "/") == 0) {
		/* GET /api/man - lista entrambe le aree */
		json = man_get_sections_json();
	} else {
		if (*path == '/') path++;

		/* Parse: area/section o area/section/name */
		char area[16] = {0};
		char section[16] = {0};

		const char *slash1 = strchr(path, '/');
		if (!slash1) {
			/* Solo area, non supportato - errore */
			json = strdup("{\"error\":\"Invalid path. Use /api/man/area/section\"}");
		} else {
			/* Estrai area */
			size_t area_len = slash1 - path;
			if (area_len >= sizeof(area)) area_len = sizeof(area) - 1;
			memcpy(area, path, area_len);

			/* Valida area */
			if (strcmp(area, "system") != 0 && strcmp(area, "packages") != 0) {
				json = strdup("{\"error\":\"Invalid area. Use 'system' or 'packages'\"}");
			} else {
				const char *rest = slash1 + 1;
				const char *slash2 = strchr(rest, '/');

				if (!slash2) {
					/* GET /api/man/area/section - lista pagine */
					json = man_get_section_pages_json(area, rest);
				} else {
					/* GET /api/man/area/section/name - metadata */
					size_t sec_len = slash2 - rest;
					if (sec_len >= sizeof(section)) sec_len = sizeof(section) - 1;
					memcpy(section, rest, sec_len);

					const char *name = slash2 + 1;
					json = man_get_page_metadata_json(area, section, name);
				}
			}
		}
	}

	if (!json) return MHD_NO;

	struct MHD_Response *res = MHD_create_response_from_buffer(
		strlen(json), json, MHD_RESPMEM_MUST_FREE);
	MHD_add_response_header(res, "Content-Type", "application/json; charset=utf-8");
	int ret = MHD_queue_response(connection, MHD_HTTP_OK, res);
	MHD_destroy_response(res);
	return ret;
}

/* * Genera i metadata JSON per una specifica pagina,
 * includendo i link ai vari formati disponibili.
 */
char *
man_get_page_metadata_json(const char *area, const char *section, const char *name)
{
	char s_clean[16], n_clean[64], a_clean[16];
	strlcpy(a_clean, area, sizeof(a_clean));
	strlcpy(s_clean, section, sizeof(s_clean));
	strlcpy(n_clean, name, sizeof(n_clean));
	sanitize(a_clean);
	sanitize(s_clean);
	sanitize(n_clean);

	char *json = malloc(2048);
	if (!json) return NULL;

	snprintf(json, 2048,
			 "{\"area\":\"%s\",\"section\":\"%s\",\"name\":\"%s\",\"formats\":[\"html\",\"md\",\"pdf\"],"
			 "\"links\":{"
			 "\"html\":\"/man/%s/%s/%s\","
			 "\"md\":\"/man/%s/%s/%s.md\","
			 "\"pdf\":\"/man/%s/%s/%s.pdf\""
			 "}}",
		  a_clean, s_clean, n_clean,
		  a_clean, s_clean, n_clean,
		  a_clean, s_clean, n_clean,
		  a_clean, s_clean, n_clean);

	return json;
}

char *
man_search_json(const char *query)
{
	char q_clean[64];
	strlcpy(q_clean, query, sizeof(q_clean));
	sanitize(q_clean);

	char cmd[512];
	/* Cerchiamo in tutte le sezioni e descrizioni */
	snprintf(cmd, sizeof(cmd), "apropos -M %s %s | head -n 50", ALL_MAN_PATHS, q_clean);

	FILE *fp = popen(cmd, "r");
	if (!fp) return NULL;

	char *json = malloc(MAX_JSON_SIZE);
	size_t len = snprintf(json, MAX_JSON_SIZE, "{\"results\":[");
	char line[1024];
	int first = 1;

	while (fgets(line, sizeof(line), fp)) {
		char *name = strtok(line, "(");
		char *section = strtok(NULL, ")");
		char *rest = strtok(NULL, "");
		char *desc = rest ? strstr(rest, "- ") : NULL;
		if (desc) desc += 2; else desc = "";

		if (name && section) {
			/* Determina l'area basandosi sul path (se possibile)
			 * Questo è un'approssimazione - apropos non dice il path esatto
			 * Per ora assumiamo 'system' come default
			 */
			const char *area = "system"; // Default

			/* Pulizia nome e sezione */
			name[strcspn(name, " \t\n\r")] = 0;
			section[strcspn(section, " \t\n\r")] = 0;
			desc[strcspn(desc, "\r\n")] = 0;

			if (!first) len += snprintf(json + len, MAX_JSON_SIZE - len, ",");

			char e_name[128], e_desc[512], e_sec[16];
			json_escape(e_name, name, sizeof(e_name));
			json_escape(e_desc, desc, sizeof(e_desc));
			json_escape(e_sec, section, sizeof(e_sec));

			len += snprintf(json + len, MAX_JSON_SIZE - len,
							"{\"name\":\"%s\",\"section\":\"%s\",\"desc\":\"%s\",\"area\":\"%s\"}",
				   e_name, e_sec, e_desc, area);
			first = 0;
		}
	}
	snprintf(json + len, MAX_JSON_SIZE - len, "]}");
	pclose(fp);
	return json;
}
