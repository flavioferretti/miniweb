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

/* Main request handler */
static int
request_handler(void *cls, struct MHD_Connection *connection, const char *url,
		const char *method, const char *version,
		const char *upload_data, size_t *upload_data_size,
		void **con_cls)
{
	(void)cls;

	/* Log request if verbose */
	if (config.verbose) {
		printf("[%s] %s %s\n", method, url, version);
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

	/* Lock unveil - no more paths can be added after this */
	if (unveil(NULL, NULL) == -1) {
		fprintf(stderr, "Warning: Cannot lock unveil\n");
	}

	/* Minimal pledge promises for web server with metrics
	 *
	 * stdio  - Standard I/O (printf, fprintf, fopen, etc.)
	 * rpath  - Read files (templates, static files)
	 * inet   - Network operations (socket, bind, accept, send, recv)
	 * proc   - Process info (getpid, getrusage - used by metrics and
	 * libmicrohttpd) exec   - Execute commands (popen for ps/netstat in
	 * metrics.c)
	 *
	 * NOTE: We need 'exec' because metrics.c uses popen() to run ps and
	 * netstat. NOTE: 'sysctl' is NOT a valid promise - it's a syscall
	 * that's allowed by default NOTE: We removed 'wpath' and 'cpath' - this
	 * server only reads, doesn't write files
	 */
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
	signal(SIGPIPE, SIG_IGN); /* Ignore SIGPIPE for network connections */
#endif

	printf("Starting MiniWeb (libmicrohttpd) on %s:%d\n", config.bind_addr,
	       config.port);

	/* Create MHD daemon */
	daemon = MHD_start_daemon(
	    MHD_USE_POLL_INTERNALLY | MHD_USE_INTERNAL_POLLING_THREAD,
	    config.port, NULL, NULL, &request_handler, NULL,
	    MHD_OPTION_THREAD_POOL_SIZE, config.thread_pool_size,
	    MHD_OPTION_CONNECTION_LIMIT, config.connection_limit,
	    MHD_OPTION_CONNECTION_TIMEOUT, 30,
	    MHD_OPTION_PER_IP_CONNECTION_LIMIT, 100, // Prevent DoS
	    MHD_OPTION_END);

	if (!daemon) {
		fprintf(stderr, "Failed to start server on port %d\n",
			config.port);
		return 1;
	}

	printf("Server started. Press Ctrl+C to stop\n");

	/* Main loop */
	while (running) {
		sleep(1);
	}

	/* Cleanup */
	MHD_stop_daemon(daemon);
	printf("\nServer stopped.\n");

	return 0;
}
