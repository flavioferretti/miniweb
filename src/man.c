#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>

#include "../include/man.h"
#include "../include/http_utils.h"

#define MAX_JSON_SIZE (256 * 1024)
#define MAX_OUTPUT_SIZE (2 * 1024 * 1024)


static int
is_valid_token(const char *s)
{
	if (!s || *s == '\0')
		return 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-' && *p != '+')
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

	char *const argv[] = {"man", "-M",
		"/usr/share/man:/usr/local/man:/usr/X11R6/man",
		"-w", (char *)section, (char *)name, NULL};
	char *path = safe_popen_read_argv("/usr/bin/man", argv, 512, 5);
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
	//size_t suffix_len = strlen(section) + 1; // Lunghezza di ".1", ".7", ecc.

	while ((de = readdir(dr)) != NULL) {
		// Salta file nascosti e cartelle (come i386, alpha)
		if (de->d_name[0] == '.') continue;
		if (de->d_type == DT_DIR) continue;

		/* * FILTRO: Il file deve terminare esattamente con .<sezione>
		 * Esempio: xterm.1 in man1 -> OK.
		 * Esempio: i386 (directory) -> Scartato da DT_DIR.
		 */
		char *dot = strrchr(de->d_name, '.');
		if (dot && strcmp(dot + 1, section) == 0) {
			char name[128];
			size_t name_len = dot - de->d_name;
			if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;

			memcpy(name, de->d_name, name_len);
			name[name_len] = '\0';

			if (!first) strlcat(json, ",", MAX_JSON_SIZE);
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
man_get_page_metadata_json(const char *area, const char *section, const char *name)
{
	char *filepath = resolve_man_path(name, section);
	if (!filepath) return strdup("{\"error\":\"Not found\"}");

	char *json = malloc(1024);
	/* Usiamo 'area' qui sotto per includerlo nel JSON */
	snprintf(json, 1024, "{\"name\":\"%s\",\"section\":\"%s\",\"area\":\"%s\",\"path\":\"%s\"}",
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
		MAX_OUTPUT_SIZE, 5);
	if (!output)
		return strdup("");

	return output;
}

/* --- Handlers HTTP (Usano http_queue_* dal tuo http_utils.h) --- */

int
man_api_handler(void *cls, struct MHD_Connection *connection, const char *url,
				const char *method, const char *version, const char *upload_data,
				size_t *upload_data_size, void **con_cls)
{
	(void)cls; (void)method; (void)version; (void)upload_data; (void)upload_data_size; (void)con_cls;
	char *json = NULL;

	/* 1. Endpoint: Elenco Sezioni (ora include system, x11, packages) */
	if (strcmp(url, "/api/man/sections") == 0 || strcmp(url, "/api/man") == 0) {
		json = man_get_sections_json();
	}
	/* 2. Endpoint: Ricerca (Apropos) */
	else if (strncmp(url, "/api/man/search/", 16) == 0) {
		json = man_api_search(url + 16);
	}
	/* 3. Endpoint: Navigazione Aree/Sezioni/Pagine */
	else {
		char area[16], section[16], name[128];

		/* Caso A: Metadati pagina specifica -> /api/man/{area}/{section}/{name} */
		if (sscanf(url, "/api/man/%15[^/]/%15[^/]/%127s", area, section, name) == 3) {
			// Validazione area per sicurezza
			if (strcmp(area, "system") == 0 || strcmp(area, "packages") == 0 || strcmp(area, "x11") == 0) {
				if (!is_valid_section(section) || !is_valid_token(name)) {
				return http_queue_400(connection, "Invalid section or page name");
			}
			json = man_get_page_metadata_json(area, section, name);
			} else {
				return http_queue_400(connection, "Invalid area: use system, packages, or x11");
			}
		}
		/* Caso B: Elenco pagine in una sezione -> /api/man/{area}/{section} */
		else if (sscanf(url, "/api/man/%15[^/]/%15[^/]", area, section) == 2) {
			if (strcmp(area, "system") == 0 || strcmp(area, "packages") == 0 || strcmp(area, "x11") == 0) {
				if (!is_valid_section(section)) {
				return http_queue_400(connection, "Invalid section");
			}
			json = man_get_section_pages_json(area, section);
			} else {
				return http_queue_400(connection, "Invalid area: use system, packages, or x11");
			}
		}
	}

	/* Gestione errore se l'URL non ha prodotto JSON o non è stato riconosciuto */
	if (!json) {
		return http_queue_404(connection, url);
	}

	/* Invio della risposta JSON */
	struct MHD_Response *response = MHD_create_response_from_buffer(
		strlen(json),
																	json,
																 MHD_RESPMEM_MUST_FREE
	);

	MHD_add_response_header(response, "Content-Type", "application/json");
	/* Opzionale: aggiungi header CORS se vuoi testare il FE da un altro server */
	MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");

	int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);

	return ret;
}

int
man_render_handler(void *cls, struct MHD_Connection *connection, const char *url,
				   const char *method, const char *version, const char *upload_data,
				   size_t *upload_data_size, void **con_cls)
{
	(void)cls; (void)method; (void)version; (void)upload_data; (void)upload_data_size; (void)con_cls;

	char area[16], section[16], page_raw[128];
	/* Parsing dell'URL: /man/area/section/page */
	if (sscanf(url, "/man/%15[^/]/%15[^/]/%127s", area, section, page_raw) != 3) {
		return http_queue_400(connection, "Formato URL non valido. Usa /man/area/sezione/pagina");
	}

	if ((strcmp(area, "system") != 0 && strcmp(area, "packages") != 0 && strcmp(area, "x11") != 0) ||
	    !is_valid_section(section) || !is_valid_token(page_raw)) {
		return http_queue_400(connection, "Invalid section or page name");
	}

	const char *mime = "text/html; charset=utf-8";
	const char *format = "html";
	char name[128];
	strlcpy(name, page_raw, sizeof(name));

	/* 1. Gestione estensioni e determinazione formato */
	char *dot = strrchr(name, '.');
	int is_markdown_request = 0;

	if (dot) {
		if (strcmp(dot, ".pdf") == 0) {
			format = "pdf";
			mime = "application/pdf";
			*dot = '\0'; /* Rimuove .pdf per la ricerca del file */
		} else if (strcmp(dot, ".ps") == 0) {
			format = "ps";
			mime = "application/postscript";
			*dot = '\0'; /* Rimuove .ps per la ricerca del file */
		} else if (strcmp(dot, ".md") == 0) {
			format = "markdown";
			mime = "text/plain; charset=utf-8"; /* Fallback a testo semplice */
			is_markdown_request = 1;
			*dot = '\0'; /* Rimuove .md per la ricerca del file */
		} else if (strcmp(dot, ".html") == 0) {
			format = "html";
			*dot = '\0'; /* Rimuove .html per la ricerca del file */
		}
	}

	/* 2. Risoluzione del path reale del manuale tramite man -w */
	/* resolve_man_path userà internamente l'area corretta per invocare man -w */
	char *filepath = resolve_man_path(name, section);
	if (!filepath) {
		return http_queue_404(connection, name);
	}

	/* 3. Esecuzione di mandoc con argv sicuro */
	char *output = NULL;
	if (is_markdown_request) {
		char *const md_argv[] = {"mandoc", "-Tmarkdown", filepath, NULL};
		output = safe_popen_read_argv("/usr/bin/mandoc", md_argv,
			MAX_OUTPUT_SIZE, 10);
		if (!output) {
			char *const ascii_argv[] = {"mandoc", "-Tascii", filepath, NULL};
			output = safe_popen_read_argv("/usr/bin/mandoc", ascii_argv,
				MAX_OUTPUT_SIZE, 10);
		}
	} else {
		char fmt[32];
		snprintf(fmt, sizeof(fmt), "-T%s", format);
		char *const argv[] = {"mandoc", fmt, filepath, NULL};
		output = safe_popen_read_argv("/usr/bin/mandoc", argv,
			MAX_OUTPUT_SIZE, 10);
	}

	free(filepath);

	/* 4. Lettura output */
	if (!output) {
		return http_queue_500(connection, "Errore durante il rendering del manuale");
	}

	/* 5. Creazione della risposta MHD */
	struct MHD_Response *response = MHD_create_response_from_buffer(
		strlen(output),
																	(void *)output,
																	MHD_RESPMEM_MUST_FREE
	);

	if (!response) {
		free(output);
		return MHD_NO;
	}

	/* Set Header Content-Type */
	MHD_add_response_header(response, "Content-Type", mime);

	/* Se è un PDF, forziamo la visualizzazione inline con nome file corretto */
	if (strcmp(format, "pdf") == 0) {
		char content_disp[128];
		snprintf(content_disp, sizeof(content_disp), "inline; filename=\"%s.pdf\"", name);
		MHD_add_response_header(response, "Content-Disposition", content_disp);
	}

	int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);

	return ret;
}
