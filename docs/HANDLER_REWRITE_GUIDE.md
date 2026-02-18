# INTEGRAZIONE COMPLETA: Handler Signature Rewrite

## Overview

Tutti gli handler sono stati riscritti per usare la nuova signature nativa kqueue:

**PRIMA** (libmicrohttpd):
```c
int handler(void *cls, struct MHD_Connection *connection, const char *url, ...);
```

**DOPO** (kqueue nativo):
```c
int handler(http_request_t *req);
```

## File da Sostituire

### 1. Header Files

```bash
# Sostituisci questi header:
cp http_handler.h include/
cp routes_new.h include/routes.h
cp urls_new.h include/urls.h
cp man_new.h include/man.h
cp metrics_new.h include/metrics.h
cp networking_new.h include/networking.h
```

### 2. Implementation Files

```bash
# Sostituisci questi file:
cp http_handler.c src/
cp routes_new.c src/routes.c
cp urls_new.c src/urls.c
cp main_kqueue.c src/main.c
```

### 3. Files da Modificare

#### A. `src/man.c` - Aggiungi funzione di rendering

Aggiungi questa nuova funzione export in `man.c`:

```c
/* man_render_page - Render man page to format */
char *
man_render_page(const char *area, const char *section, 
                const char *page, const char *format)
{
	char cmd[512];
	const char *mandoc_format;
	
	/* Map format to mandoc flag */
	if (strcmp(format, "html") == 0) {
		mandoc_format = "html";
	} else if (strcmp(format, "pdf") == 0) {
		mandoc_format = "pdf";
	} else if (strcmp(format, "ps") == 0) {
		mandoc_format = "ps";
	} else if (strcmp(format, "md") == 0) {
		mandoc_format = "markdown";
	} else {
		return NULL;
	}
	
	/* Build mandoc command */
	snprintf(cmd, sizeof(cmd),
	         "/usr/bin/mandoc -T%s /usr/share/man/%s/%s/%s.%s 2>/dev/null",
	         mandoc_format, area, section, page, section);
	
	/* Execute and return */
	return safe_popen_read(cmd, 1024 * 1024); /* 1MB limit */
}
```

Gli handler `man_render_handler` e `man_api_handler` sono già in `routes_new.c`.

## Makefile Updates

### Prima (con libmicrohttpd):
```makefile
LIBS = -lmicrohttpd -lm -lpthread
```

### Dopo (solo pthread):
```makefile
LIBS = -lm -lpthread
```

### Aggiungi http_handler.o:
```makefile
OBJS = build/main.o \
       build/http_handler.o \
       build/routes.o \
       build/template_engine.o \
       build/metrics.o \
       build/man.o \
       build/http_utils.o \
       build/urls.o \
       build/networking.o

# Add compilation rule
build/http_handler.o: src/http_handler.c include/http_handler.h
	$(CC) $(CFLAGS) -c src/http_handler.c -o build/http_handler.o
```

## Nuove API per Handler

### http_request_t - Request Info

```c
typedef struct http_request {
	int fd;                          /* Client socket */
	const char *method;              /* GET, POST, etc */
	const char *url;                 /* Request path */
	const char *version;             /* HTTP/1.1 */
	const char *buffer;              /* Full request buffer */
	size_t buffer_len;               /* Buffer length */
	struct sockaddr_in *client_addr; /* Client address */
} http_request_t;
```

### Funzioni Helper

```c
/* Get header value */
const char *http_request_get_header(http_request_t *req, const char *name);

/* Get client IP (with X-Real-IP/X-Forwarded-For support) */
const char *http_request_get_client_ip(http_request_t *req);

/* Check if HTTPS (via X-Forwarded-Proto) */
int http_request_is_https(http_request_t *req);
```

### Quick Response Functions

```c
/* Send error */
int http_send_error(http_request_t *req, int status_code, const char *message);

/* Send JSON */
int http_send_json(http_request_t *req, const char *json);

/* Send HTML */
int http_send_html(http_request_t *req, const char *html);

/* Send file */
int http_send_file(http_request_t *req, const char *path, const char *content_type);
```

### Advanced Response Building

```c
/* Create response */
http_response_t *resp = http_response_create();

/* Set status */
http_response_set_status(resp, 200);

/* Add header */
http_response_add_header(resp, "Cache-Control", "no-cache");

/* Set body (with auto-free) */
http_response_set_body(resp, json, strlen(json), 1);

/* Send */
http_response_send(req, resp);

/* Cleanup */
http_response_free(resp);
```

## Esempio Handler

### Vecchio Stile (libmicrohttpd):

```c
int
dashboard_handler(void *cls, struct MHD_Connection *connection,
                 const char *url, const char *method,
                 const char *version, const char *upload_data,
                 size_t *upload_data_size, void **con_cls)
{
	(void)cls; (void)url; (void)method; (void)version;
	(void)upload_data; (void)upload_data_size; (void)con_cls;
	
	char *html = template_render("dashboard.html", "Dashboard", NULL, NULL);
	
	struct MHD_Response *response = MHD_create_response_from_buffer(
		strlen(html), html, MHD_RESPMEM_MUST_FREE);
	
	MHD_add_response_header(response, "Content-Type", "text/html");
	int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);
	
	return ret;
}
```

### Nuovo Stile (kqueue nativo):

```c
int
dashboard_handler(http_request_t *req)
{
	struct template_data data = {
		.title = "MiniWeb - Dashboard",
		.page_content = "dashboard.html",
		.extra_head_file = "dashboard_extra_head.html",
		.extra_js_file = "dashboard_extra_js.html"
	};
	
	char *output = NULL;
	template_render_with_data(&data, &output);
	
	http_response_t *resp = http_response_create();
	http_response_set_body(resp, output, strlen(output), 1);
	
	int ret = http_response_send(req, resp);
	http_response_free(resp);
	
	return ret;
}
```

## Testing

```bash
# Compile
make clean && make

# Should compile WITHOUT libmicrohttpd!
# Binary size should be ~200KB instead of ~1MB

# Run
./build/miniweb -v

# Test all endpoints
curl http://127.0.0.1:9001/
curl http://127.0.0.1:9001/api/metrics
curl http://127.0.0.1:9001/api/networking
curl http://127.0.0.1:9001/networking
curl http://127.0.0.1:9001/docs
curl http://127.0.0.1:9001/static/css/custom.css

# Load test
ab -n 10000 -c 100 http://127.0.0.1:9001/
wrk -t4 -c100 -d30s http://127.0.0.1:9001/api/metrics
```

## Benefici

1. ✅ **Zero external dependencies** - Solo libc + pthread
2. ✅ **Binario piccolissimo** - ~200KB vs ~1MB
3. ✅ **Più veloce** - kqueue nativo, no overhead libmicrohttpd
4. ✅ **Codice più pulito** - Handler semplificati
5. ✅ **Più OpenBSD-style** - Filosofia minimalista nativa
6. ✅ **DoS protection** - Hard limit su connessioni
7. ✅ **Thread-safe** - Mutex su connection pool
8. ✅ **Stesso CLI** - Nessun cambio per gli utenti

## Struttura Handler

Tutti gli handler seguono questo pattern:

```c
int handler_name(http_request_t *req)
{
	/* 1. Collect data */
	char *data = get_some_data();
	
	/* 2. Create response */
	http_response_t *resp = http_response_create();
	resp->content_type = "application/json";
	
	/* 3. Add headers if needed */
	http_response_add_header(resp, "Cache-Control", "no-cache");
	
	/* 4. Set body */
	http_response_set_body(resp, data, strlen(data), 1);
	
	/* 5. Send */
	int ret = http_response_send(req, resp);
	
	/* 6. Cleanup */
	http_response_free(resp);
	
	return ret;
}
```

## Checklist Integrazione

- [ ] Sostituisci tutti gli header (6 files)
- [ ] Sostituisci src/main.c
- [ ] Sostituisci src/routes.c
- [ ] Sostituisci src/urls.c
- [ ] Aggiungi src/http_handler.c
- [ ] Aggiungi `man_render_page()` in src/man.c
- [ ] Aggiorna Makefile (rimuovi -lmicrohttpd, aggiungi http_handler.o)
- [ ] Compila: `make clean && make`
- [ ] Testa tutti gli endpoint
- [ ] Verifica dimensione binario (<300KB)
- [ ] Load test per stabilità

## Note Importanti

1. **Template Engine**: Nessun cambio necessario - già compatibile
2. **Metrics Collection**: Nessun cambio - già thread-safe
3. **Man System**: Aggiungi solo `man_render_page()` 
4. **Networking**: Gli handler sono già in routes_new.c
5. **Static Files**: Gestiti da http_send_file() in http_handler.c

## Troubleshooting

**Problema**: `undefined reference to MHD_*`
**Soluzione**: Hai dimenticato di rimuovere `-lmicrohttpd` dal Makefile

**Problema**: Handler non trovati
**Soluzione**: Verifica che `init_routes()` in urls.c registri tutti i percorsi

**Problema**: 503 immediatamente
**Soluzione**: MAX_CONNECTIONS troppo basso o leak di connessioni

**Problema**: Segfault
**Soluzione**: Verifica che tutti i puntatori in http_request_t siano validi
