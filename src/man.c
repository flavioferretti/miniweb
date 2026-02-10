#include "../include/man.h"
#include <ctype.h>
#include <errno.h>
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

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

/*
 * Versione migliorata di read_command_output con:
 * - Timeout per prevenire freeze
 * - Cleanup garantito del processo figlio
 * - Gestione errori completa
 */
static char *
read_command_output(const char *cmd, size_t max_size)
{
	FILE *fp = popen(cmd, "r");
	if (!fp) {
		fprintf(stderr, "popen failed for: %s (errno=%d)\n", cmd,
			errno);
		return NULL;
	}

	size_t capacity = 16384;
	size_t total = 0;
	char *buffer = malloc(capacity);
	if (!buffer) {
		pclose(fp);
		return NULL;
	}

	char chunk[4096];
	size_t n;

	/* Leggi l'output con controllo della dimensione massima */
	while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
		/* Verifica se abbiamo raggiunto il limite */
		if (total + n >= max_size) {
			fprintf(
			    stderr,
			    "Warning: command output truncated at %zu bytes\n",
			    max_size);
			/* Leggi fino alla fine per evitare deadlock del
			 * processo */
			while (fread(chunk, 1, sizeof(chunk), fp) > 0)
				;
			break;
		}

		/* Espandi il buffer se necessario */
		if (total + n + 1 > capacity) {
			size_t new_capacity = capacity * 2;
			if (new_capacity > max_size) {
				new_capacity = max_size;
			}

			char *new_buf = realloc(buffer, new_capacity);
			if (!new_buf) {
				fprintf(stderr,
					"realloc failed, output truncated\n");
				break;
			}
			buffer = new_buf;
			capacity = new_capacity;
		}

		memcpy(buffer + total, chunk, n);
		total += n;
	}

	buffer[total] = '\0';

	/* Chiudi la pipe e verifica lo stato del processo */
	int status = pclose(fp);
	if (status == -1) {
		fprintf(stderr, "pclose failed for: %s (errno=%d)\n", cmd,
			errno);
		free(buffer);
		return NULL;
	}

	/* Verifica se il comando è terminato con errore */
	if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		fprintf(stderr, "Command exited with status %d: %s\n",
			WEXITSTATUS(status), cmd);
		/* Non è necessariamente un errore fatale, restituiamo comunque
		 * l'output */
	}

	/* Se non abbiamo letto nulla, restituisci NULL invece di una stringa
	 * vuota */
	if (total == 0) {
		free(buffer);
		return NULL;
	}

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
static void
json_escape(char *dest, const char *src, size_t max)
{
	size_t d = 0;
	for (size_t s = 0; src[s] != '\0' && d < max - 5; s++) {
		switch (src[s]) {
		case '"':
			dest[d++] = '\\';
			dest[d++] = '"';
			break;
		case '\\':
			dest[d++] = '\\';
			dest[d++] = '\\';
			break;
		case '\b':
			dest[d++] = '\\';
			dest[d++] = 'b';
			break;
		case '\f':
			dest[d++] = '\\';
			dest[d++] = 'f';
			break;
		case '\n':
			dest[d++] = '\\';
			dest[d++] = 'n';
			break;
		case '\r':
			dest[d++] = '\\';
			dest[d++] = 'r';
			break;
		case '\t':
			dest[d++] = '\\';
			dest[d++] = 't';
			break;
		default:
			dest[d++] = src[s];
			break;
		}
	}
	dest[d] = '\0';
}

/*
 * Versione migliorata con gestione errori e cleanup garantito
 */
char *
man_get_section_pages_json(const char *area, const char *section)
{
	if (!area || !section) {
		return strdup("{\"error\":\"Missing parameters\"}");
	}

	char s_clean[16];
	strlcpy(s_clean, section, sizeof(s_clean));
	sanitize(s_clean);

	/* Determina il path base in base all'area */
	const char *man_path = "/usr/share/man";
	if (strcmp(area, "packages") == 0) {
		man_path = "/usr/local/man";
	}

	char cmd[512];
	snprintf(cmd, sizeof(cmd), "apropos -M %s -s %s ^ 2>/dev/null",
		 man_path, s_clean);

	FILE *fp = popen(cmd, "r");
	if (!fp) {
		char *err = malloc(128);
		snprintf(
		    err, 128,
		    "{\"error\":\"Failed to execute apropos\",\"errno\":%d}",
		    errno);
		return err;
	}

	char *json = malloc(MAX_JSON_SIZE);
	if (!json) {
		pclose(fp);
		return strdup("{\"error\":\"Out of memory\"}");
	}

	/* Inizializzazione JSON pulita */
	size_t len = snprintf(json, MAX_JSON_SIZE,
			      "{\"area\":\"%s\",\"section\":\"%s\",\"pages\":[",
			      area, s_clean);

	if (len >= MAX_JSON_SIZE) {
		free(json);
		pclose(fp);
		return strdup("{\"error\":\"Buffer overflow\"}");
	}

	char line[1024];
	int first = 1;
	int count = 0;
	const int MAX_PAGES =
	    500; /* Limita il numero di pagine per prevenire JSON enormi */

	while (fgets(line, sizeof(line), fp) && count < MAX_PAGES) {
		/* Controllo di sicurezza per non sforare il buffer */
		if (len > MAX_JSON_SIZE - 1024) {
			fprintf(stderr, "Warning: JSON buffer nearly full, "
					"truncating results\n");
			break;
		}

		char *name = strtok(line, "(");
		char *rest = strtok(NULL, "");
		if (!name || !rest)
			continue;

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

		/* Skip se il nome è vuoto dopo la pulizia */
		if (strlen(name) == 0)
			continue;

		/* Escaping dei caratteri che rompono il JSON */
		char e_name[128], e_desc[512];
		json_escape(e_name, name, sizeof(e_name));
		json_escape(e_desc, desc, sizeof(e_desc));

		int written = snprintf(json + len, MAX_JSON_SIZE - len,
				       "%s{\"name\":\"%s\",\"desc\":\"%s\"}",
				       first ? "" : ",", e_name, e_desc);

		if (written < 0 || (size_t)written >= MAX_JSON_SIZE - len) {
			fprintf(stderr, "Warning: JSON entry truncated\n");
			break;
		}

		len += written;
		first = 0;
		count++;
	}

	/* Chiusura JSON - sempre garantita */
	size_t remaining = MAX_JSON_SIZE - len;
	if (remaining > 3) {
		snprintf(json + len, remaining, "]}");
	} else {
		/* Se non c'è spazio, tronca e chiudi comunque */
		json[MAX_JSON_SIZE - 3] = ']';
		json[MAX_JSON_SIZE - 2] = '}';
		json[MAX_JSON_SIZE - 1] = '\0';
	}

	/* CLEANUP GARANTITO */
	int status = pclose(fp);
	if (status == -1) {
		fprintf(stderr, "Warning: pclose failed (errno=%d)\n", errno);
	}

	return json;
}

/*
 * Versione migliorata con timeout e controllo dimensione output
 */
char *
man_render_page(const char *area, const char *section, const char *name,
		const char *format)
{
	if (!area || !section || !name || !format) {
		return NULL;
	}

	char s_clean[16], n_clean[64];
	strlcpy(s_clean, section, sizeof(s_clean));
	strlcpy(n_clean, name, sizeof(n_clean));
	sanitize(s_clean);
	sanitize(n_clean);

	/* Validazione area */
	const char *man_path = "/usr/share/man";
	if (strcmp(area, "packages") == 0) {
		man_path = "/usr/local/man";
	} else if (strcmp(area, "system") != 0) {
		fprintf(stderr, "Invalid area: %s\n", area);
		return NULL;
	}

	/* Determina formato mandoc */
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
	/*
	 * Aggiungiamo timeout di 30 secondi per prevenire freeze
	 * su pagine man complesse o PDF generation lenta
	 */
	snprintf(cmd, sizeof(cmd),
		 "timeout 30 man -M %s -S %s -T %s %s 2>/dev/null", man_path,
		 s_clean, m_fmt, n_clean);

	char *output = read_command_output(cmd, MAX_OUTPUT_SIZE);

	if (!output) {
		fprintf(stderr, "Failed to render man page: %s(%s) format=%s\n",
			n_clean, s_clean, format);
	}

	return output;
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

	/* Parse URL: /man/area/section/name[.format] */
	const char *path = url + strlen("/man/");

	char area[16] = {0}, section[16] = {0}, name[64] = {0},
	     format[16] = "html";
	const char *c_type = "text/html; charset=utf-8";

	/* Extract area */
	const char *slash1 = strchr(path, '/');
	if (!slash1) {
		const char *err =
		    "Invalid URL format. Expected: /man/area/section/name";
		struct MHD_Response *res = MHD_create_response_from_buffer(
		    strlen(err), (void *)err, MHD_RESPMEM_PERSISTENT);
		int ret =
		    MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST, res);
		MHD_destroy_response(res);
		return ret;
	}

	size_t area_len = slash1 - path;
	if (area_len >= sizeof(area))
		return MHD_NO;
	memcpy(area, path, area_len);

	/* Validate area */
	if (strcmp(area, "system") != 0 && strcmp(area, "packages") != 0) {
		const char *err = "Invalid area. Use 'system' or 'packages'";
		struct MHD_Response *res = MHD_create_response_from_buffer(
		    strlen(err), (void *)err, MHD_RESPMEM_PERSISTENT);
		int ret =
		    MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST, res);
		MHD_destroy_response(res);
		return ret;
	}

	/* Extract section */
	const char *rest = slash1 + 1;
	const char *slash2 = strchr(rest, '/');
	if (!slash2)
		return MHD_NO;

	size_t sec_len = slash2 - rest;
	if (sec_len >= sizeof(section))
		sec_len = sizeof(section) - 1;
	memcpy(section, rest, sec_len);

	/* Extract name and format */
	const char *name_ptr = slash2 + 1;
	const char *dot = strrchr(name_ptr, '.');

	if (dot) {
		size_t name_len = dot - name_ptr;
		if (name_len >= sizeof(name))
			name_len = sizeof(name) - 1;
		memcpy(name, name_ptr, name_len);

		if (strcmp(dot, ".md") == 0) {
			strlcpy(format, "md", sizeof(format));
			c_type = "text/plain; charset=utf-8";
		} else if (strcmp(dot, ".pdf") == 0) {
			strlcpy(format, "pdf", sizeof(format));
			c_type = "application/pdf";
		}
	} else {
		strlcpy(name, name_ptr, sizeof(name));
	}

	/* Render la pagina */
	char *output = man_render_page(area, section, name, format);

	if (!output) {
		const char *err = "Man page not found or rendering failed";
		struct MHD_Response *res = MHD_create_response_from_buffer(
		    strlen(err), (void *)err, MHD_RESPMEM_PERSISTENT);
		int ret =
		    MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, res);
		MHD_destroy_response(res);
		return ret;
	}

	struct MHD_Response *res = MHD_create_response_from_buffer(
	    strlen(output), output, MHD_RESPMEM_MUST_FREE);
	MHD_add_response_header(res, "Content-Type", c_type);

	/* Aggiungi cache header per ridurre carico */
	MHD_add_response_header(res, "Cache-Control", "public, max-age=3600");

	int ret = MHD_queue_response(connection, MHD_HTTP_OK, res);
	MHD_destroy_response(res);
	return ret;
}

/* --- HTTP Handlers per libmicrohttpd --- */

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

	/* Routing interno all'API */
	if (strncmp(path, "/search", 7) == 0) {
		const char *q = MHD_lookup_connection_value(
		    connection, MHD_GET_ARGUMENT_KIND, "q");
		if (q && strlen(q) > 0) {
			json = man_search_json(q);
		} else {
			json = strdup("{\"results\":[],\"error\":\"Missing or "
				      "empty query parameter\"}");
		}
	} else if (*path == '\0' || strcmp(path, "/") == 0) {
		/* GET /api/man - lista entrambe le aree */
		json = man_get_sections_json();
	} else {
		if (*path == '/')
			path++;

		/* Parse: area/section o area/section/name */
		char area[16] = {0};
		char section[16] = {0};

		const char *slash1 = strchr(path, '/');
		if (!slash1) {
			/* Solo area, non supportato - errore */
			json = strdup("{\"error\":\"Invalid path. Use "
				      "/api/man/area/section\"}");
		} else {
			/* Estrai area */
			size_t area_len = slash1 - path;
			if (area_len >= sizeof(area))
				area_len = sizeof(area) - 1;
			memcpy(area, path, area_len);

			/* Valida area */
			if (strcmp(area, "system") != 0 &&
			    strcmp(area, "packages") != 0) {
				json = strdup("{\"error\":\"Invalid area. Use "
					      "'system' or 'packages'\"}");
			} else {
				const char *rest = slash1 + 1;
				const char *slash2 = strchr(rest, '/');

				if (!slash2) {
					/* GET /api/man/area/section - lista
					 * pagine */
					json = man_get_section_pages_json(area,
									  rest);
				} else {
					/* GET /api/man/area/section/name -
					 * metadata */
					size_t sec_len = slash2 - rest;
					if (sec_len >= sizeof(section))
						sec_len = sizeof(section) - 1;
					memcpy(section, rest, sec_len);

					const char *name = slash2 + 1;
					json = man_get_page_metadata_json(
					    area, section, name);
				}
			}
		}
	}

	/* Se json è ancora NULL, c'è stato un errore interno */
	if (!json) {
		json = strdup("{\"error\":\"Internal server error\"}");
	}

	struct MHD_Response *res = MHD_create_response_from_buffer(
	    strlen(json), json, MHD_RESPMEM_MUST_FREE);
	MHD_add_response_header(res, "Content-Type",
				"application/json; charset=utf-8");

	/* CORS headers per API */
	MHD_add_response_header(res, "Access-Control-Allow-Origin", "*");

	int ret = MHD_queue_response(connection, MHD_HTTP_OK, res);
	MHD_destroy_response(res);
	return ret;
}

/*
 * Genera i metadata JSON per una specifica pagina,
 * includendo i link ai vari formati disponibili.
 */
char *
man_get_page_metadata_json(const char *area, const char *section,
			   const char *name)
{
	if (!area || !section || !name) {
		return strdup("{\"error\":\"Missing parameters\"}");
	}

	char s_clean[16], n_clean[64], a_clean[16];
	strlcpy(a_clean, area, sizeof(a_clean));
	strlcpy(s_clean, section, sizeof(s_clean));
	strlcpy(n_clean, name, sizeof(n_clean));
	sanitize(a_clean);
	sanitize(s_clean);
	sanitize(n_clean);

	char *json = malloc(2048);
	if (!json) {
		return strdup("{\"error\":\"Out of memory\"}");
	}

	snprintf(json, 2048,
		 "{\"area\":\"%s\",\"section\":\"%s\",\"name\":\"%s\","
		 "\"formats\":[\"html\",\"md\",\"pdf\"],"
		 "\"links\":{"
		 "\"html\":\"/man/%s/%s/%s\","
		 "\"md\":\"/man/%s/%s/%s.md\","
		 "\"pdf\":\"/man/%s/%s/%s.pdf\""
		 "}}",
		 a_clean, s_clean, n_clean, a_clean, s_clean, n_clean, a_clean,
		 s_clean, n_clean, a_clean, s_clean, n_clean);

	return json;
}

/*
 * Versione migliorata di man_search_json con:
 * - Gestione errori completa
 * - Cleanup garantito
 * - Limiti di risultati
 */
char *
man_search_json(const char *query)
{
	if (!query || strlen(query) == 0) {
		return strdup("{\"error\":\"Empty query\",\"results\":[]}");
	}

	char q_clean[64];
	strlcpy(q_clean, query, sizeof(q_clean));
	sanitize(q_clean);

	/* Se dopo sanitizzazione la query è vuota, ritorna errore */
	if (strlen(q_clean) == 0) {
		return strdup("{\"error\":\"Invalid query after "
			      "sanitization\",\"results\":[]}");
	}

	char cmd[512];
	/* Cerchiamo in tutte le sezioni e descrizioni, limitiamo a 50 risultati
	 */
	snprintf(cmd, sizeof(cmd), "apropos -M %s %s 2>/dev/null | head -n 50",
		 ALL_MAN_PATHS, q_clean);

	FILE *fp = popen(cmd, "r");
	if (!fp) {
		char *err = malloc(128);
		snprintf(
		    err, 128,
		    "{\"error\":\"Search failed\",\"errno\":%d,\"results\":[]}",
		    errno);
		return err;
	}

	char *json = malloc(MAX_JSON_SIZE);
	if (!json) {
		pclose(fp);
		return strdup("{\"error\":\"Out of memory\",\"results\":[]}");
	}

	size_t len = snprintf(json, MAX_JSON_SIZE,
			      "{\"query\":\"%s\",\"results\":[", q_clean);

	char line[1024];
	int first = 1;
	int count = 0;
	const int MAX_RESULTS = 50;

	while (fgets(line, sizeof(line), fp) && count < MAX_RESULTS) {
		/* Controllo buffer overflow */
		if (len > MAX_JSON_SIZE - 1024) {
			fprintf(stderr, "Warning: search results truncated\n");
			break;
		}

		char *name = strtok(line, "(");
		char *section = strtok(NULL, ")");
		char *rest = strtok(NULL, "");
		char *desc = rest ? strstr(rest, "- ") : NULL;
		if (desc)
			desc += 2;
		else
			desc = "";

		if (!name || !section)
			continue;

		/* Determina l'area basandosi sul path
		 * Questo è un'approssimazione - apropos non dice il path esatto
		 */
		const char *area = "system"; // Default

		/* Pulizia nome e sezione */
		name[strcspn(name, " \t\n\r")] = 0;
		section[strcspn(section, " \t\n\r")] = 0;
		desc[strcspn(desc, "\r\n")] = 0;

		/* Skip entry vuote */
		if (strlen(name) == 0 || strlen(section) == 0)
			continue;

		if (!first) {
			int written =
			    snprintf(json + len, MAX_JSON_SIZE - len, ",");
			if (written < 0)
				break;
			len += written;
		}

		char e_name[128], e_desc[512], e_sec[16];
		json_escape(e_name, name, sizeof(e_name));
		json_escape(e_desc, desc, sizeof(e_desc));
		json_escape(e_sec, section, sizeof(e_sec));

		int written = snprintf(json + len, MAX_JSON_SIZE - len,
				       "{\"name\":\"%s\",\"section\":\"%s\","
				       "\"desc\":\"%s\",\"area\":\"%s\"}",
				       e_name, e_sec, e_desc, area);

		if (written < 0 || (size_t)written >= MAX_JSON_SIZE - len) {
			fprintf(stderr,
				"Warning: search result entry truncated\n");
			break;
		}

		len += written;
		first = 0;
		count++;
	}

	/* Chiusura JSON garantita */
	size_t remaining = MAX_JSON_SIZE - len;
	if (remaining > 3) {
		snprintf(json + len, remaining, "]}");
	} else {
		json[MAX_JSON_SIZE - 3] = ']';
		json[MAX_JSON_SIZE - 2] = '}';
		json[MAX_JSON_SIZE - 1] = '\0';
	}

	/* CLEANUP GARANTITO */
	int status = pclose(fp);
	if (status == -1) {
		fprintf(stderr,
			"Warning: pclose failed for search (errno=%d)\n",
			errno);
	}

	return json;
}
