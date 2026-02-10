/* main.c - Punto di ingresso per OpenBSD con libmicrohttpd */

#include <microhttpd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/routes.h"

/* OpenBSD security headers */
#ifdef _OPENBSD
#include <unistd.h>
#endif

/* Configuration */
struct server_config {
	int port;
	const char *bind_addr;
	int thread_pool_size;
	int connection_limit;
	int verbose;
};

static struct server_config config = {.port = 9001,
				      .bind_addr = "127.0.0.1",
				      .thread_pool_size = 4,
				      .connection_limit = 1000,
				      .verbose = 0};

static volatile sig_atomic_t running = 1;

/* Signal handler */
static void
handle_signal(int sig)
{
	(void)sig;
	running = 0;
}

/*
 * Estrae l'IP reale del client dai header del reverse proxy
 * Controlla in ordine: X-Real-IP, X-Forwarded-For, poi fallback all'IP diretto
 */
static const char *
get_real_client_ip(struct MHD_Connection *connection)
{
	const char *real_ip = MHD_lookup_connection_value(
	    connection, MHD_HEADER_KIND, "X-Real-IP");

	if (real_ip) {
		return real_ip;
	}

	const char *forwarded = MHD_lookup_connection_value(
	    connection, MHD_HEADER_KIND, "X-Forwarded-For");

	if (forwarded) {
		/* X-Forwarded-For può contenere una lista: "client, proxy1,
		 * proxy2" Prendiamo solo il primo IP */
		static char first_ip[64];
		const char *comma = strchr(forwarded, ',');
		if (comma) {
			size_t len = comma - forwarded;
			if (len >= sizeof(first_ip))
				len = sizeof(first_ip) - 1;
			memcpy(first_ip, forwarded, len);
			first_ip[len] = '\0';
			return first_ip;
		}
		return forwarded;
	}

	/* Fallback: usa l'IP della connessione diretta */
	const union MHD_ConnectionInfo *info = MHD_get_connection_info(
	    connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);

	if (info && info->client_addr) {
		/* Per semplicità restituiamo una stringa statica
		 * In produzione convertirei sockaddr in stringa IP */
		return "(direct connection)";
	}

	return "unknown";
}

/*
 * Determina se la richiesta è arrivata via HTTPS
 * Controlla X-Forwarded-Proto dal reverse proxy
 */
static int
is_https_request(struct MHD_Connection *connection)
{
	const char *proto = MHD_lookup_connection_value(
	    connection, MHD_HEADER_KIND, "X-Forwarded-Proto");

	return (proto && strcmp(proto, "https") == 0);
}

/* Main request handler */
static int
request_handler(void *cls, struct MHD_Connection *connection, const char *url,
		const char *method, const char *version,
		const char *upload_data, size_t *upload_data_size,
		void **con_cls)
{
	(void)cls;

	/* Log request if verbose - ora con IP reale */
	if (config.verbose) {
		const char *client_ip = get_real_client_ip(connection);
		const char *proto =
		    is_https_request(connection) ? "HTTPS" : "HTTP";
		printf("[%s] %s %s %s (from %s)\n", proto, method, url, version,
		       client_ip);
	}

	/* Find route handler */
	route_handler_t handler = route_match(method, url);

	if (!handler) {
		/* No handler found */
		const char *not_found =
		    "<html><body><h1>404 Not Found</h1></body></html>";
		struct MHD_Response *response = MHD_create_response_from_buffer(
		    strlen(not_found), (void *)not_found,
		    MHD_RESPMEM_PERSISTENT);

		MHD_add_response_header(response, "Content-Type", "text/html");
		int ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND,
					     response);
		MHD_destroy_response(response);
		return ret;
	}

	/* Call the handler */
	return handler(NULL, connection, url, method, version, upload_data,
		       upload_data_size, con_cls);
}

/* Print usage */
static void
usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [options]\n", progname);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -p PORT      Port to listen on (default: %d)\n",
		config.port);
	fprintf(stderr, "  -b ADDR      Address to bind to (default: %s)\n",
		config.bind_addr);
	fprintf(stderr, "  -t NUM       Thread pool size (default: %d)\n",
		config.thread_pool_size);
	fprintf(stderr, "  -c NUM       Max connections (default: %d)\n",
		config.connection_limit);
	fprintf(stderr, "  -v           Enable verbose output\n");
	fprintf(stderr, "  -h           Show this help\n");
}

/* Parse command line arguments */
static void
parse_args(int argc, char *argv[])
{
	int opt;

	while ((opt = getopt(argc, argv, "p:b:t:c:vh")) != -1) {
		switch (opt) {
		case 'p':
			config.port = atoi(optarg);
			break;
		case 'b':
			config.bind_addr = optarg;
			break;
		case 't':
			config.thread_pool_size = atoi(optarg);
			break;
		case 'c':
			config.connection_limit = atoi(optarg);
			break;
		case 'v':
			config.verbose = 1;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			usage(argv[0]);
			exit(1);
		}
	}
}

/* Apply OpenBSD security features */
static void
apply_openbsd_security(void)
{
	printf("Applying OpenBSD security features...\n");

	/* Unveil directories for reading templates and static files */
	if (unveil("templates", "r") == -1) {
		fprintf(stderr, "Warning: Cannot unveil templates directory\n");
	}

	if (unveil("static", "r") == -1) {
		fprintf(stderr, "Warning: Cannot unveil static directory\n");
	}

	/* Unveil system binaries for ps and netstat (needed by metrics.c) */
	if (unveil("/bin", "x") == -1) {
		fprintf(stderr, "Warning: Cannot unveil /bin directory\n");
	}

	if (unveil("/usr/bin", "x") == -1) {
		fprintf(stderr, "Warning: Cannot unveil /usr/bin directory\n");
	}

	// Accesso ai manuali
	if (unveil("/usr/share/man", "r") == -1) {
		fprintf(stderr,
			"Warning: Cannot unveil /usr/share/man directory\n");
	}
	if (unveil("/usr/local/man", "r") == -1) {
		fprintf(stderr,
			"Warning: Cannot unveil /usr/local/man directory\n");
	}

	// Binari necessari per popen()
	if (unveil("/usr/bin/mandoc", "x") == -1) {
		fprintf(stderr, "Warning: Cannot unveil /usr/bin/mandoc\n");
	}

	if (unveil("/usr/bin/man", "x") == -1) {
		fprintf(stderr, "Warning: Cannot unveil /usr/bin/man\n");
	}

	if (unveil("/usr/bin/less", "x") == -1) {
		fprintf(stderr, "Warning: Cannot unveil /usr/bin/less\n");
	}

	if (unveil("/usr/bin/apropos", "x") == -1) {
		fprintf(stderr, "Warning: Cannot unveil /usr/bin/apropos\n");
	}
	if (unveil("/bin/sh", "x") == -1) {
		fprintf(stderr, "Warning: Cannot unveil /bin/sh\n");
	}

	// File di configurazione di apropos
	if (unveil("/etc/man.conf", "r") == -1) {
		fprintf(stderr, "Warning: Cannot unveil /etc/man.conf\n");
	}

	/* Lock unveil - no more paths can be added after this */
	if (unveil(NULL, NULL) == -1) {
		fprintf(stderr, "Warning: Cannot lock unveil\n");
	}

	/* Minimal pledge promises for web server with metrics */
	const char *promises = "stdio rpath inet proc exec vminfo ps";

	if (pledge(promises, NULL) == -1) {
		perror("pledge");
		fprintf(stderr, "Failed to pledge: %s\n", promises);
		fprintf(stderr, "Continuing without pledge...\n");
	} else {
		if (config.verbose) {
			printf("Pledge promises set: %s\n", promises);
		}
	}
}

/* Main function */
int
main(int argc, char *argv[])
{
	struct MHD_Daemon *daemon = NULL;

	/* Parse arguments */
	parse_args(argc, argv);

	/* Initialize routes */
	init_routes();

	/* Apply OpenBSD security */
	apply_openbsd_security();

	/* Set up signal handlers */
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif

	printf("Starting MiniWeb (libmicrohttpd) on %s:%d\n", config.bind_addr,
	       config.port);

	/*
	 * CONFIGURAZIONE OTTIMIZZATA PER REVERSE PROXY
	 *
	 * IMPORTANTE: Non possiamo usare MHD_USE_POLL con
	 * MHD_USE_THREAD_PER_CONNECTION perché non sono compatibili nella
	 * versione di libmicrohttpd su OpenBSD.
	 *
	 * Usiamo invece MHD_USE_INTERNAL_POLLING_THREAD con un thread pool.
	 * Questo offre un buon compromesso tra performance e compatibilità.
	 *
	 * Differenze dalla configurazione precedente:
	 * 1. MHD_USE_INTERNAL_POLLING_THREAD: un thread gestisce tutte le
	 * connessioni
	 * 2. THREAD_POOL_SIZE: crea un pool di worker thread per gestire le
	 * richieste
	 * 3. Timeout aumentato a 120 secondi per operazioni lente
	 * 4. Connection limits per prevenire DoS
	 */
	daemon = MHD_start_daemon(
	    MHD_USE_INTERNAL_POLLING_THREAD, /* Un thread gestisce poll() */
	    config.port, NULL, NULL, &request_handler, NULL,

	    /* Thread pool per distribuire il carico di lavoro */
	    MHD_OPTION_THREAD_POOL_SIZE, config.thread_pool_size,

	    /* Limiti di connessione per prevenire DoS */
	    MHD_OPTION_CONNECTION_LIMIT, config.connection_limit,
	    MHD_OPTION_PER_IP_CONNECTION_LIMIT, 20,

	    /* Timeout aumentato per operazioni lente (man, apropos, PDF) */
	    MHD_OPTION_CONNECTION_TIMEOUT, 120,

	    /* Buffer size ottimizzato */
	    MHD_OPTION_CONNECTION_MEMORY_LIMIT, 256 * 1024,

	    MHD_OPTION_END);

	if (!daemon) {
		fprintf(stderr, "Failed to start server on port %d\n",
			config.port);
		fprintf(stderr, "\nPossible causes:\n");
		fprintf(
		    stderr,
		    "  - Port already in use (check with: fstat | grep :%d)\n",
		    config.port);
		fprintf(stderr, "  - Permission denied (try ports > 1024)\n");
		fprintf(stderr, "  - IPv6/IPv4 binding issue\n");
		return 1;
	}

	printf("Server started successfully!\n");
	if (config.verbose) {
		printf("Configuration:\n");
		printf("  - Mode: Internal polling thread with worker pool\n");
		printf("  - Thread pool size: %d\n", config.thread_pool_size);
		printf("  - Max connections: %d\n", config.connection_limit);
		printf("  - Connection timeout: 120s\n");
		printf("  - Per-IP limit: 20\n");
		printf("  - Listening on: %s:%d\n", config.bind_addr,
		       config.port);
	}
	printf("Press Ctrl+C to stop\n\n");

	/* Main loop */
	while (running) {
		sleep(1);
	}

	/* Cleanup */
	printf("\nShutting down...\n");
	MHD_stop_daemon(daemon);
	printf("Server stopped.\n");

	return 0;
}
