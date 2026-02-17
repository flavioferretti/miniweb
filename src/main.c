/* main.c - kqueue-based HTTP server for OpenBSD */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <err.h>
#include <pthread.h>


#include "../include/config.h"
#include "../include/http_handler.h"
#include "../include/http_utils.h"
#include "../include/routes.h"
#include "../include/urls.h"

#define MAX_EVENTS 64  /* Aggiungi questa se manca */

/* Configuration */
#define MAX_CONNECTIONS 1280       /* DoS protection: max simultaneous connections */
#define THREAD_POOL_SIZE 4        /* Number of worker threads */
#define REQUEST_BUFFER_SIZE 16384 /* 16KB per request */
#define LISTEN_BACKLOG 128

struct server_config {
	int port;
	const char *bind_addr;
	int thread_pool_size;
	int connection_limit;
	int verbose;
};

static struct server_config config = {
	.port = 9001,
	.bind_addr = "127.0.0.1",
	.thread_pool_size = THREAD_POOL_SIZE,
	.connection_limit = MAX_CONNECTIONS,
	.verbose = 0
};

/* Define the global verbose flag for other modules */
int config_verbose = 0;

static volatile sig_atomic_t running = 1;
static int kq_fd = -1;
static int listen_fd = -1;

/* Connection state */
typedef struct connection {
	int fd;
	struct sockaddr_in addr;
	char buffer[REQUEST_BUFFER_SIZE];
	size_t bytes_read;
	time_t created;        /* wall-clock accept time, for idle timeout */
	unsigned int gen;      /* generation counter — incremented on free,
	* so stale kevent udata pointers are detectable */
} connection_t;

#define CONN_TIMEOUT_SEC 30    /* close idle connections after 30 s */

/* Connection pool — indexed directly by fd for O(1) lookup.
 * Valid fd range: 3 .. MAX_CONNECTIONS-1.
 * (0,1,2 are stdin/stdout/stderr and are never client sockets.) */
static connection_t *connections[MAX_CONNECTIONS];
static unsigned int  conn_gen[MAX_CONNECTIONS]; /* per-slot generation */
static pthread_mutex_t conn_mutex = PTHREAD_MUTEX_INITIALIZER;
static int active_connections = 0;

/* Thread pool */
typedef struct {
	pthread_t thread;
	int id;
	int kq_fd;  // Add kq_fd to worker struct
} worker_t;

static worker_t workers[THREAD_POOL_SIZE];

/* Signal handler */
static void
handle_signal(int sig)
{
	(void)sig;
	running = 0;
}

/* Set socket to non-blocking mode */
static void
set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1) flags = 0;
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Allocate a connection slot for fd.
 * Returns the new connection_t* or NULL if the pool is full.
 * Caller must NOT hold conn_mutex. */
connection_t *
alloc_connection(int fd, struct sockaddr_in *addr)
{
	if (fd < 0 || fd >= MAX_CONNECTIONS)
		return NULL;

	pthread_mutex_lock(&conn_mutex);

	if (active_connections >= MAX_CONNECTIONS ||
		connections[fd] != NULL) {
		/* fd already occupied — shouldn't happen, but guard it */
		pthread_mutex_unlock(&conn_mutex);
	return NULL;
		}

		connection_t *conn = calloc(1, sizeof(connection_t));
		if (!conn) {
			pthread_mutex_unlock(&conn_mutex);
			return NULL;
		}

		conn->fd      = fd;
		conn->created = time(NULL);
		conn->gen     = conn_gen[fd]; /* matches current slot generation */
		if (addr)
			memcpy(&conn->addr, addr, sizeof(struct sockaddr_in));

	connections[fd] = conn;
	active_connections++;

	pthread_mutex_unlock(&conn_mutex);
	return conn;
}

/* Release the connection slot for fd.
 * Bumps the generation counter so any in-flight kevent udata pointers
 * for this fd become detectable as stale.
 * Caller must NOT hold conn_mutex. */
void
free_connection(int fd)
{
	if (fd < 0 || fd >= MAX_CONNECTIONS)
		return;

	pthread_mutex_lock(&conn_mutex);

	if (connections[fd]) {
		free(connections[fd]);
		connections[fd] = NULL;
		conn_gen[fd]++;          /* invalidate stale pointers */
		if (active_connections > 0)
			active_connections--;
	}

	pthread_mutex_unlock(&conn_mutex);
}

/* Parse HTTP request line into method/url/version.
 * Returns 0 on success, -1 on parse error. */
static int
parse_request_line(const char *buffer, char *method, char *url, char *version)
{
	const char *line_end = strstr(buffer, "\r\n");
	if (!line_end)
		return -1;

	size_t line_len = (size_t)(line_end - buffer);

	char line[2048];
	if (line_len >= sizeof(line))
		return -1;

	memcpy(line, buffer, line_len);
	line[line_len] = '\0';

	/* method ≤ 31 chars, url ≤ 511 chars, version ≤ 31 chars */
	int n = sscanf(line, "%31s %511s %31s", method, url, version);
	if (n != 3)
		return -1;

	return 0;
}

/* Send HTTP response */
static void
send_response(int fd, int status_code, const char *status_text,
			  const char *content_type, const char *body, size_t body_len)
{
	char header[2048];
	int header_len;

	header_len = snprintf(header, sizeof(header),
						  "HTTP/1.1 %d %s\r\n"
						  "Content-Type: %s\r\n"
						  "Content-Length: %zu\r\n"
						  "Connection: close\r\n"
						  "Server: MiniWeb/kqueue\r\n"
						  "\r\n",
					   status_code, status_text, content_type, body_len);

	/* Send header */
	write(fd, header, header_len);

	/* Send body */
	if (body && body_len > 0) {
		write(fd, body, body_len);
	}
}

/* Send error response */
static void
send_error(int fd, int status_code, const char *message)
{
	char body[1024];
	int body_len;

	body_len = snprintf(body, sizeof(body),
						"<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
						"<title>Code %d </title>"
						"<link rel=\"stylesheet\" type=\"text/css\" href=\"/static/css/custom.css\" />"
						"</head><body>"
						"<h1>%d - Page Not Found</h1>"
						"<p>The requested resource <code>%s</code> was not found on this "
						"server.</p>"
						"<hr><p><a href=\"/\">MiniWeb Server</a> on OpenBSD</p>"
						"</body></html>",
					 status_code, status_code, message);

	const char *status_text;
	switch (status_code) {
		case 400: status_text = "Bad Request"; break;
		case 403: status_text = "Forbidden"; break;
		case 404: status_text = "Not Found"; break;
		case 500: status_text = "Internal Server Error"; break;
		case 503: status_text = "Service Unavailable"; break;
		default:  status_text = "Error"; break;
	}

	send_response(fd, status_code, status_text, "text/html; charset=utf-8",
				  body, body_len);
}

/* Dispatch request to handler */
static void
dispatch_request(connection_t *conn, const char *method, const char *url,
				 const char *version)
{
	/* Find route handler */
	http_handler_t handler = find_route_match(method, url);

	if (!handler) {
		send_error(conn->fd, 404, "Not Found");
		return;
	}

	/* Create http_request_t - FIXED: using addr instead of client_addr */
	http_request_t req = {
		.fd = conn->fd,
		.method = method,
		.url = url,
		.version = version,
		.buffer = conn->buffer,
		.buffer_len = conn->bytes_read,
		.client_addr = &conn->addr  /* This matches connection_t field name */
	};

	/* Call handler */
	handler(&req);
}

/* Worker thread — processes read-ready events from the shared kqueue. */
void *
worker_thread(void *arg)
{
	worker_t *worker = (worker_t *)arg;
	struct kevent events[MAX_EVENTS];

	while (running) {
		/* 1-second timeout so we can sweep idle connections */
		struct timespec ts = {1, 0};
		int nevents = kevent(worker->kq_fd, NULL, 0,
							 events, MAX_EVENTS, &ts);
		if (nevents < 0) {
			if (errno == EINTR) continue;
			break;
		}

		/* ── Idle timeout sweep (worker 0 only, once per second) ── */
		if (worker->id == 0) {
			time_t now = time(NULL);
			pthread_mutex_lock(&conn_mutex);
			for (int j = 0; j < MAX_CONNECTIONS; j++) {
				connection_t *c = connections[j];
				if (c && (now - c->created) > CONN_TIMEOUT_SEC) {
					close(c->fd);
					free(c);
					connections[j] = NULL;
					conn_gen[j]++;
					if (active_connections > 0)
						active_connections--;
				}
			}
			pthread_mutex_unlock(&conn_mutex);
		}

		for (int i = 0; i < nevents; i++) {
			int fd = (int)events[i].ident;

			if (fd == listen_fd)
				continue;

			if (fd < 0 || fd >= MAX_CONNECTIONS)
				continue;

			/* ── Generation check: detect stale udata pointers ── */
			connection_t *conn = (connection_t *)events[i].udata;
			if (!conn)
				continue;

			pthread_mutex_lock(&conn_mutex);
			int stale = (connections[fd] != conn ||
			conn->gen != conn_gen[fd]);
			pthread_mutex_unlock(&conn_mutex);

			if (stale)
				continue;

			/* ── EOF / error ── */
			if (events[i].flags & (EV_EOF | EV_ERROR)) {
				close(fd);
				free_connection(fd);
				continue;
			}

			if (events[i].filter != EVFILT_READ)
				continue;

			/* ── Read ── */
			ssize_t n = recv(fd,
							 conn->buffer + conn->bytes_read,
					REQUEST_BUFFER_SIZE - conn->bytes_read - 1, 0);

			if (n <= 0) {
				if (n < 0 && (errno == EAGAIN ||
					errno == EWOULDBLOCK))
					continue;
				close(fd);
				free_connection(fd);
				continue;
			}

			conn->bytes_read += (size_t)n;
			conn->buffer[conn->bytes_read] = '\0';

			if (strstr(conn->buffer, "\r\n\r\n") == NULL) {
				continue; /* request not yet complete */
			}

			char method[32]  = {0};
			char path[512]   = {0};
			char version[32] = {0};

			if (parse_request_line(conn->buffer,
				method, path, version) == 0) {
				dispatch_request(conn, method, path, version);
				} else {
					http_request_t req = {
						.fd          = fd,
						.method      = "",
						.url         = "",
						.version     = "",
						.buffer      = conn->buffer,
						.buffer_len  = conn->bytes_read,
						.client_addr = &conn->addr
					};
					http_send_error(&req, 400, "Bad Request");
				}

				close(fd);
				free_connection(fd);
		}
	}
	return NULL;
}


/* Print usage */
static void
usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [options]\n", progname);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -p PORT      Port to listen on (default: %d)\n", config.port);
	fprintf(stderr, "  -b ADDR      Address to bind to (default: %s)\n", config.bind_addr);
	fprintf(stderr, "  -t NUM       Thread pool size (default: %d)\n", config.thread_pool_size);
	fprintf(stderr, "  -c NUM       Max connections (default: %d)\n", config.connection_limit);
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
				if (config.thread_pool_size > THREAD_POOL_SIZE) {
					config.thread_pool_size = THREAD_POOL_SIZE;
				}
				break;
			case 'c':
				config.connection_limit = atoi(optarg);
				if (config.connection_limit > MAX_CONNECTIONS) {
					config.connection_limit = MAX_CONNECTIONS;
				}
				break;
			case 'v':
				config.verbose = 1;
				config_verbose = 1;
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
	unveil("/etc/passwd", "r");
	unveil("/etc/group", "r");
	unveil("/etc/resolv.conf", "r");
	unveil(NULL, NULL);

	/* Note: 'route' removed as NET_RT_DUMP is problematic with pledge */
	const char *promises = "stdio rpath inet route proc exec vminfo ps getpw";
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
	struct sockaddr_in bind_addr, client_addr;
	socklen_t client_len;
	struct kevent change;

	parse_args(argc, argv);

	/* Initialize routes (make sure this function exists) */
	init_routes();

	/* Set up signal handlers */
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
	#endif

	printf("Starting MiniWeb (kqueue) on %s:%d\n",
		   config.bind_addr, config.port);

	/* Create listen socket */
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		err(1, "socket");
	}

	int opt = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	set_nonblock(listen_fd);

	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_port = htons(config.port);

	if (inet_pton(AF_INET, config.bind_addr, &bind_addr.sin_addr) != 1) {
		fprintf(stderr, "Invalid bind address: %s\n", config.bind_addr);
		return 1;
	}

	if (bind(listen_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		err(1, "bind");
	}

	if (listen(listen_fd, LISTEN_BACKLOG) < 0) {
		err(1, "listen");
	}

	/* Create kqueue */
	kq_fd = kqueue();
	if (kq_fd < 0) {
		err(1, "kqueue");
	}

	/* Monitor listen socket */
	EV_SET(&change, listen_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
	if (kevent(kq_fd, &change, 1, NULL, 0, NULL) < 0) {
		err(1, "kevent add listen");
	}

	/* Create worker threads */
	for (int i = 0; i < config.thread_pool_size; i++) {
		workers[i].id = i;
		workers[i].kq_fd = kq_fd;  // Pass kq_fd to worker
		pthread_create(&workers[i].thread, NULL, worker_thread, &workers[i]);
	}

	apply_openbsd_security();

	printf("Server started successfully!\n");
	if (config.verbose) {
		printf("  - Thread pool size: %d\n", config.thread_pool_size);
		printf("  - Max connections: %d\n", config.connection_limit);
		printf("  - Listening on: %s:%d\n", config.bind_addr, config.port);
	}
	printf("Press Ctrl+C to stop\n\n");

	/* Main accept loop */
	while (running) {
		struct timespec timeout = {1, 0}; /* 1 second */
		struct kevent events[1];

		int n = kevent(kq_fd, NULL, 0, events, 1, &timeout);

		if (n < 0) {
			if (errno == EINTR) continue;
			break;
		}

		if (n == 0) continue; /* Timeout */

			/* Accept new connection */
			if (events[0].ident == (unsigned int)listen_fd) {
				client_len = sizeof(client_addr);
				int conn_fd = accept(listen_fd,
									 (struct sockaddr *)&client_addr, &client_len);

				if (conn_fd < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK) {
						continue;
					}
					if (config.verbose) {
						perror("accept");
					}
					continue;
				}

				set_nonblock(conn_fd);

				/* Allocate connection */
				connection_t *conn = alloc_connection(conn_fd, &client_addr);
				if (!conn) {
					/* Connection limit reached - send 503 */
					send_error(conn_fd, 503,
							   "Server busy - connection limit reached");
					close(conn_fd);

					if (config.verbose) {
						printf("Connection limit reached, rejected fd=%d\n", conn_fd);
					}
					continue;
				}

				/* Add to kqueue for reading */
				EV_SET(&change, conn_fd, EVFILT_READ, EV_ADD, 0, 0, conn);
				if (kevent(kq_fd, &change, 1, NULL, 0, NULL) < 0) {
					close(conn_fd);
					free_connection(conn_fd);
					continue;
				}

				if (config.verbose) {
					char ip[INET_ADDRSTRLEN];
					inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
					printf("New connection: fd=%d from %s (active: %d)\n",
						   conn_fd, ip, active_connections);
				}
			}
	}

	printf("\nShutting down...\n");

	/* Stop worker threads */
	running = 0;
	for (int i = 0; i < config.thread_pool_size; i++) {
		pthread_join(workers[i].thread, NULL);
	}

	/* Close all connections */
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (connections[i]) {
			close(connections[i]->fd);
			free(connections[i]);
		}
	}

	close(listen_fd);
	close(kq_fd);

	printf("Server stopped.\n");
	return 0;
}
