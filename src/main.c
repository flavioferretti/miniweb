/* main.c - Entry point for OpenBSD con libmicrohttpd */

#include <arpa/inet.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../include/config.h"
#include "../include/http_utils.h"
#include "../include/routes.h"

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

/* Define the global verbose flag for other modules */
int config_verbose = 0;  /* Add this line */

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

	return "127.0.0.1";
}

/*
 * Determina se la richiesta è arrivata via HTTPS
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

	if (config.verbose) {
		const char *client_ip = get_real_client_ip(connection);
		const char *proto =
		    is_https_request(connection) ? "HTTPS" : "HTTP";
		printf("[%s] %s %s %s (from %s)\n", proto, method, url, version,
		       client_ip);
	}

	route_handler_t handler = route_match(method, url);
	if (!handler) {
		return http_queue_404(connection, url);
	}

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
			config_verbose = 1;  /* Add this line */
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
#ifdef __OpenBSD__
	printf("Applying OpenBSD security features...\n");

	unveil("templates", "r");
	unveil("static", "r");
	unveil("/usr/share/man", "r");
	unveil("/usr/local/man", "r");
	unveil("/usr/X11R6/man", "r");
	unveil("/usr/bin/mandoc", "x");
	unveil("/usr/bin/man", "x");
	unveil("/usr/bin/apropos", "x");
	unveil("/bin/ps", "x");
	unveil("/usr/bin/netstat", "x");
	unveil("/bin/sh", "x");
	unveil("/etc/man.conf", "r");
	unveil(NULL, NULL);

	const char *promises = "stdio rpath inet proc exec vminfo ps";
	if (pledge(promises, NULL) == -1) {
		perror("pledge");
		fprintf(stderr, "Continuing without pledge...\n");
	} else if (config.verbose) {
		printf("Pledge promises set: %s\n", promises);
	}
#else
	if (config.verbose)
		printf("OpenBSD security features disabled on this platform.\n");
#endif
}

/* Main function */
int
main(int argc, char *argv[])
{
	struct MHD_Daemon *daemon = NULL;

	parse_args(argc, argv);
	init_routes();
	apply_openbsd_security();

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif

	printf("Starting MiniWeb (libmicrohttpd) on %s:%d\n", config.bind_addr,
	       config.port);

	struct sockaddr_in bind_addr;
	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_port = htons(config.port);

	if (inet_pton(AF_INET, config.bind_addr, &bind_addr.sin_addr) != 1) {
		fprintf(stderr, "Invalid bind address: %s\n", config.bind_addr);
		return 1;
	}

	daemon = MHD_start_daemon(
	    MHD_USE_POLL | MHD_USE_INTERNAL_POLLING_THREAD, config.port, NULL,
	    NULL, &request_handler, NULL, MHD_OPTION_SOCK_ADDR,
	    (struct sockaddr *)&bind_addr, MHD_OPTION_THREAD_POOL_SIZE,
	    config.thread_pool_size, MHD_OPTION_CONNECTION_LIMIT,
	    config.connection_limit, MHD_OPTION_PER_IP_CONNECTION_LIMIT, 20,
	    MHD_OPTION_CONNECTION_TIMEOUT, 120,
	    MHD_OPTION_CONNECTION_MEMORY_LIMIT, 256 * 1024, MHD_OPTION_END);

	if (!daemon) {
		fprintf(stderr, "Failed to start server on port %d\n",
			config.port);
		fprintf(stderr,
			"Check if port is already in use (fstat | grep :%d)\n",
			config.port);
		return 1;
	}

	printf("Server started successfully!\n");
	if (config.verbose) {
		printf("  - Thread pool size: %d\n", config.thread_pool_size);
		printf("  - Max connections: %d\n", config.connection_limit);
		printf("  - Listening on: %s:%d\n", config.bind_addr,
		       config.port);
	}
	printf("Press Ctrl+C to stop\n\n");

	while (running) {
		sleep(1);
	}

	printf("\nShutting down...\n");
	MHD_stop_daemon(daemon);
	printf("Server stopped.\n");

	return 0;
}
