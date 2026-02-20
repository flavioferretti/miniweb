/* main.c - kqueue-based HTTP server for OpenBSD
 *
 * Architecture: single-threaded dispatcher (kqueue) + worker thread pool.
 *
 *   Main thread  →  kevent() loop (accept + enqueue read-ready fds)
 *   Worker N     →  dequeue connection → recv → parse → dispatch → close
 *
 * Key design choices:
 *   - EV_DISPATCH: auto-disables the event after delivery, eliminating the
 *     race where multiple workers could receive the same fd.
 *   - Work queue with pthread_cond_wait: zero busy-waiting in workers.
 *   - Connection pool indexed by fd: O(1) alloc/free, generation counter
 *     to detect stale udata pointers from already-closed fds.
 *   - Idle timeout swept by the main thread (no worker needed for it).
 */

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
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <err.h>

#include "../include/conf.h"
#include "../include/config.h"
#include "../include/http_handler.h"
#include "../include/http_utils.h"
#include "../include/routes.h"
#include "../include/urls.h"

/* -- Compile-time hard limits (not overridable at runtime) ------------------ */
#define MAX_EVENTS          64
#define MAX_CONNECTIONS     1280
#define THREAD_POOL_SIZE    4
#define REQUEST_BUFFER_SIZE 16384
#define LISTEN_BACKLOG      128
#define QUEUE_CAPACITY      512
#define MAX_KEEPALIVE_REQUESTS 64

/* -- Active configuration (populated in main before any thread starts) ------ */
static miniweb_conf_t config;

int config_verbose = 0;   /* consulted by other translation units */
char config_static_dir[CONF_STR_MAX] = "static";
char config_templates_dir[CONF_STR_MAX] = "templates";

static volatile sig_atomic_t running = 1;
static int kq_fd     = -1;
static int listen_fd = -1;

/* -- Connection pool -------------------------------------------------------- */
typedef struct connection {
	int              fd;
	struct sockaddr_in addr;
	char             buffer[REQUEST_BUFFER_SIZE];
	size_t           bytes_read;
	time_t           created;
	time_t           last_activity;
	int              requests_served;
	unsigned int     gen;     /* matches conn_gen[fd] at alloc time */
} connection_t;

static connection_t  *connections[MAX_CONNECTIONS];
static unsigned int   conn_gen[MAX_CONNECTIONS];
static pthread_mutex_t conn_mutex = PTHREAD_MUTEX_INITIALIZER;
static int            active_connections = 0;

/* -- Work queue ------------------------------------------------------------- */
typedef struct {
	connection_t *items[QUEUE_CAPACITY];
	int           head;
	int           tail;
	int           count;
	pthread_mutex_t lock;
	pthread_cond_t  not_empty;
	pthread_cond_t  not_full;
} work_queue_t;

static work_queue_t wq;

static void
queue_init(work_queue_t *q)
{
	memset(q, 0, sizeof(*q));
	pthread_mutex_init(&q->lock, NULL);
	pthread_cond_init(&q->not_empty, NULL);
	pthread_cond_init(&q->not_full, NULL);
}

/* Enqueue from the main thread. Non-blocking: returns 0 on success, -1 if
 * the queue is full (connection will be dropped). */
static int
queue_push(work_queue_t *q, connection_t *conn)
{
	pthread_mutex_lock(&q->lock);
	if (q->count >= QUEUE_CAPACITY) {
		pthread_mutex_unlock(&q->lock);
		return -1;
	}
	q->items[q->tail] = conn;
	q->tail = (q->tail + 1) % QUEUE_CAPACITY;
	q->count++;
	pthread_cond_signal(&q->not_empty);
	pthread_mutex_unlock(&q->lock);
	return 0;
}

/* Dequeue in a worker thread. Blocks until an item is available or
 * running becomes 0. Returns NULL on shutdown. */
static connection_t *
queue_pop(work_queue_t *q)
{
	pthread_mutex_lock(&q->lock);
	while (q->count == 0 && running) {
		pthread_cond_wait(&q->not_empty, &q->lock);
	}
	if (q->count == 0) {          /* running == 0 and queue empty */
		pthread_mutex_unlock(&q->lock);
		return NULL;
	}
	connection_t *conn = q->items[q->head];
	q->head = (q->head + 1) % QUEUE_CAPACITY;
	q->count--;
	pthread_mutex_unlock(&q->lock);
	return conn;
}

/* Wake all workers so they can notice running == 0 and exit cleanly. */
static void
queue_broadcast_shutdown(work_queue_t *q)
{
	pthread_mutex_lock(&q->lock);
	pthread_cond_broadcast(&q->not_empty);
	pthread_mutex_unlock(&q->lock);
}

/* -- Connection pool helpers ------------------------------------------------ */
static connection_t *
alloc_connection(int fd, struct sockaddr_in *addr)
{
	if (fd < 0 || fd >= MAX_CONNECTIONS)
		return NULL;

	pthread_mutex_lock(&conn_mutex);

	if (active_connections >= config.max_conns ||
		connections[fd] != NULL) {
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
		conn->last_activity = conn->created;
		conn->gen     = conn_gen[fd];
		if (addr)
			memcpy(&conn->addr, addr, sizeof(struct sockaddr_in));

	connections[fd] = conn;
	active_connections++;

	pthread_mutex_unlock(&conn_mutex);
	return conn;
}

static void
free_connection(int fd)
{
	if (fd < 0 || fd >= MAX_CONNECTIONS)
		return;

	pthread_mutex_lock(&conn_mutex);
	if (connections[fd]) {
		free(connections[fd]);
		connections[fd] = NULL;
		conn_gen[fd]++;
		if (active_connections > 0)
			active_connections--;
	}
	pthread_mutex_unlock(&conn_mutex);
}

/* -- Helpers ---------------------------------------------------------------- */
static void
set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1) flags = 0;
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int
parse_request_line(const char *buf, char *method, char *url, char *version)
{
	const char *sp1 = strchr(buf, ' ');
	if (!sp1 || sp1 == buf)
		return -1;

	const char *sp2 = strchr(sp1 + 1, ' ');
	if (!sp2 || sp2 == sp1 + 1)
		return -1;

	const char *eol = strstr(sp2 + 1, "\r\n");
	if (!eol || eol == sp2 + 1)
		return -1;

	size_t mlen = (size_t)(sp1 - buf);
	size_t ulen = (size_t)(sp2 - (sp1 + 1));
	size_t vlen = (size_t)(eol - (sp2 + 1));

	if (mlen >= 32 || ulen >= 512 || vlen >= 32)
		return -1;

	memcpy(method, buf, mlen);
	method[mlen] = '\0';
	memcpy(url, sp1 + 1, ulen);
	url[ulen] = '\0';
	memcpy(version, sp2 + 1, vlen);
	version[vlen] = '\0';

	return 0;
}

static const char *
find_header_end(const char *buf)
{
	return strstr(buf, "\r\n\r\n");
}

static int
request_keep_alive(const char *buf, const char *version)
{
	int is_http11 = (strcmp(version, "HTTP/1.1") == 0);
	const char *p = buf;

	while ((p = strcasestr(p, "\r\nConnection:")) != NULL) {
		p += 13;
		while (*p == ' ' || *p == '\t')
			p++;
		if (strncasecmp(p, "close", 5) == 0)
			return 0;
		if (strncasecmp(p, "keep-alive", 10) == 0)
			return 1;
	}

	if (!strncasecmp(buf, "Connection:", 11)) {
		p = buf + 11;
		while (*p == ' ' || *p == '\t')
			p++;
		if (strncasecmp(p, "close", 5) == 0)
			return 0;
		if (strncasecmp(p, "keep-alive", 10) == 0)
			return 1;
	}

	return is_http11 ? 1 : 0;
}

static void
send_error_response(int fd, int code, const char *msg)
{
	http_request_t req = {
		.fd = fd,
		.method = "GET",
		.url = "/",
		.version = "HTTP/1.1",
		.keep_alive = 0,
	};

	(void)http_send_error(&req, code, msg);
}

static int
try_rearm_keepalive(connection_t *conn)
{
	if (!conn)
		return 0;
	if (conn->requests_served >= MAX_KEEPALIVE_REQUESTS)
		return 0;

	conn->requests_served++;
	conn->bytes_read = 0;
	conn->buffer[0] = '\0';
	conn->last_activity = time(NULL);

	struct kevent ev;
	EV_SET(&ev, conn->fd, EVFILT_READ, EV_ENABLE, 0, 0, conn);
	return kevent(kq_fd, &ev, 1, NULL, 0, NULL) == 0;
}

/* -- Worker thread ---------------------------------------------------------- */
/*
 * Each worker blocks on queue_pop(), processes one connection at a time,
 * then loops. No kevent() calls here — only the main thread touches kqueue.
 */
static void *
worker_thread(void *arg)
{
	(void)arg;   /* workers are identical; id not needed */

	while (running) {
		connection_t *conn = queue_pop(&wq);
		if (!conn) {
			break;   /* shutdown signal */
		}

		int fd = conn->fd;
		int close_conn = 1;

		/* -- Read loop: accumulate until we have a full HTTP request ---- */
		int request_done = 0;
		while (!request_done) {
			ssize_t n = recv(fd,
							 conn->buffer + conn->bytes_read,
					(size_t)config.max_req_size - conn->bytes_read - 1,
							 0);
			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					/* No data yet — re-arm the kqueue event and bail.
					 * EV_ENABLE re-enables the EVFILT_READ (was auto-
					 * disabled by EV_DISPATCH on first delivery). */
					struct kevent ev;
					EV_SET(&ev, fd, EVFILT_READ, EV_ENABLE, 0, 0, conn);
					kevent(kq_fd, &ev, 1, NULL, 0, NULL);
					goto next_conn;   /* do NOT close */
				}
				/* Real error → fall through to close */
				break;
			}
			if (n == 0) {
				break;   /* peer closed */
			}

			conn->bytes_read += (size_t)n;
			conn->last_activity = time(NULL);
			conn->buffer[conn->bytes_read] = '\0';

			if (find_header_end(conn->buffer)) {
				request_done = 1;
			} else if (conn->bytes_read >= (size_t)config.max_req_size - 1) {
				/* Request too large */
				send_error_response(fd, 400, "Request Too Large");
				break;
			} else {
				/* Partial read: try again (socket is non-blocking, so if
				 * there's nothing available recv() returns EAGAIN above) */
				continue;
			}
		}

		if (request_done) {
			char method[32]  = {0};
			char path[512]   = {0};
			char version[32] = {0};

			if (parse_request_line(conn->buffer,
				method, path, version) == 0) {
				int keep_alive = request_keep_alive(conn->buffer, version);
				http_handler_t handler = route_match(method, path);
			if (handler) {
				http_request_t req = {
					.fd          = fd,
					.method      = method,
					.url         = path,
					.version     = version,
					.keep_alive  = keep_alive,
					.buffer      = conn->buffer,
					.buffer_len  = conn->bytes_read,
					.client_addr = &conn->addr,
				};
				handler(&req);
				keep_alive = req.keep_alive;
				if (keep_alive && try_rearm_keepalive(conn))
					close_conn = 0;
			} else {
				http_request_t req = {
					.fd          = fd,
					.method      = method,
					.url         = path,
					.version     = version,
					.keep_alive  = keep_alive,
					.buffer      = conn->buffer,
					.buffer_len  = conn->bytes_read,
					.client_addr = &conn->addr,
				};
				http_send_error(&req, 404, "Not Found");
				if (req.keep_alive && try_rearm_keepalive(conn))
					close_conn = 0;
			}
				} else {
					send_error_response(fd, 400, "Bad Request");
				}
		}

		if (close_conn) {
			close(fd);
			free_connection(fd);
		}

		next_conn:;
	}

	return NULL;
}

/* -- Accept helper (called from main loop) ---------------------------------- */
static void
handle_accept(void)
{
	for (;;) {   /* drain all pending connections in one go */
		struct sockaddr_in caddr;
		socklen_t          clen = sizeof(caddr);

		int cfd = accept(listen_fd,
						 (struct sockaddr *)&caddr, &clen);
		if (cfd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return;   /* no more pending connections */
			}
			if (errno == EINTR)
				continue;
			if (config.verbose)
				perror("accept");
			return;
		}

		set_nonblock(cfd);

		connection_t *conn = alloc_connection(cfd, &caddr);
		if (!conn) {
			send_error_response(cfd, 503, "Server busy");
			close(cfd);
			if (config.verbose)
				fprintf(stderr, "Connection limit reached, rejected fd=%d\n", cfd);
			continue;
		}

		/* EV_DISPATCH: deliver once, then auto-disable.
		 * This prevents multiple workers from receiving the same fd. */
		struct kevent ev;
		EV_SET(&ev, cfd, EVFILT_READ, EV_ADD | EV_DISPATCH, 0, 0, conn);
		if (kevent(kq_fd, &ev, 1, NULL, 0, NULL) < 0) {
			close(cfd);
			free_connection(cfd);
			continue;
		}

		if (config.verbose) {
			char ip[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
			fprintf(stderr, "New connection: fd=%d from %s (active: %d)\n",
					cfd, ip, active_connections);
		}
	}
}

/* -- Idle timeout sweep ----------------------------------------------------- */
static void
sweep_idle_connections(void)
{
	time_t now = time(NULL);
	pthread_mutex_lock(&conn_mutex);
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		connection_t *c = connections[i];
		if (c && (now - c->last_activity) > config.conn_timeout) {
			close(c->fd);
			free(c);
			connections[i] = NULL;
			conn_gen[i]++;
			if (active_connections > 0)
				active_connections--;
		}
	}
	pthread_mutex_unlock(&conn_mutex);
}

/* -- Signal handler --------------------------------------------------------- */
static void
handle_signal(int sig)
{
	(void)sig;
	running = 0;
}

/* -- CLI -------------------------------------------------------------------- */
static void
usage(const char *prog)
{
	fprintf(stderr,
			"Usage: %s [options]\n"
			"  -f FILE   Config file (default: auto-detect)\n"
			"  -p PORT   Port (default %d)\n"
			"  -b ADDR   Bind address (default %s)\n"
			"  -t NUM    Worker threads (default %d, max %d)\n"
			"  -c NUM    Max connections (default %d)\n"
			"  -v        Verbose\n"
			"  -h        Help\n",
		 prog,
		 config.port, config.bind_addr,
		 config.threads, THREAD_POOL_SIZE,
		 config.max_conns);
}

static void
parse_args(int argc, char *argv[])
{
	/* Raw CLI values — -1/NULL means "not supplied". */
	const char *conf_file  = NULL;
	int         cli_port   = -1;
	const char *cli_bind   = NULL;
	int         cli_threads  = -1;
	int         cli_conns    = -1;
	int         cli_verbose  = 0;

	int opt;
	while ((opt = getopt(argc, argv, "f:p:b:t:c:vh")) != -1) {
		switch (opt) {
			case 'f': conf_file   = optarg;      break;
			case 'p': cli_port    = atoi(optarg); break;
			case 'b': cli_bind    = optarg;       break;
			case 't': cli_threads = atoi(optarg); break;
			case 'c': cli_conns   = atoi(optarg); break;
			case 'v': cli_verbose = 1;            break;
			case 'h': usage(argv[0]); exit(0);
			default:  usage(argv[0]); exit(1);
		}
	}

	/* 1. Compiled-in defaults */
	conf_defaults(&config);

	/* 2. Config file (overwrites defaults for keys that are present) */
	if (conf_load(conf_file, &config) != 0)
		exit(1);

	/* 3. CLI flags (highest priority — overwrite everything) */
	conf_apply_cli(&config, cli_port, cli_bind,
				   cli_threads, cli_conns, cli_verbose);

	/* Clamp to hard limits */
	if (config.threads  < 1)               config.threads  = 1;
	if (config.threads  > THREAD_POOL_SIZE) config.threads  = THREAD_POOL_SIZE;
	if (config.max_conns > MAX_CONNECTIONS) config.max_conns = MAX_CONNECTIONS;

	/* Propagate config to global values consulted by other modules */
	config_verbose = config.verbose;
	strlcpy(config_static_dir, config.static_dir, sizeof(config_static_dir));
	strlcpy(config_templates_dir, config.templates_dir, sizeof(config_templates_dir));

	if (config.verbose)
		conf_dump(&config);
}

/* -- OpenBSD security ------------------------------------------------------- */
static void
apply_openbsd_security(void)
{
	#ifdef __OpenBSD__
	fprintf(stderr, "Applying OpenBSD security features...\n");

	/* Filesystem paths from config */
	unveil(config.templates_dir, "r");
	unveil(config.static_dir,    "r");

	/* Man page infrastructure */
	unveil("/usr/share/man",    "r");
	unveil("/usr/local/man",    "r");
	unveil("/usr/X11R6/man",    "r");
	unveil(config.mandoc_path,  "x");
	unveil("/usr/bin/man",      "x");
	unveil("/usr/bin/apropos",  "x");
	unveil("/bin/ps",           "x");
	unveil("/usr/bin/netstat",  "x");
	unveil("/bin/sh",           "x");
	unveil("/etc/man.conf",     "r");
	unveil("/dev/null", "rw");

	/* User/group lookups */
	unveil("/etc/passwd",       "r");
	unveil("/etc/group",        "r");
	unveil("/etc/resolv.conf",  "r");

	unveil(NULL, NULL);

	const char *promises =
	"stdio rpath inet route proc exec vminfo ps getpw";
	if (pledge(promises, NULL) == -1) {
		perror("pledge");
		fprintf(stderr, "Continuing without pledge...\n");
	} else if (config.verbose) {
		fprintf(stderr, "Pledge promises set: %s\n", promises);
	}
	#else
	if (config.verbose)
		fprintf(stderr, "OpenBSD security features disabled on this platform.\n");
	#endif
}

/* -- main ------------------------------------------------------------------- */
int
main(int argc, char *argv[])
{
	parse_args(argc, argv);
	init_routes();

	signal(SIGINT,  handle_signal);
	signal(SIGTERM, handle_signal);
	#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
	#endif

	printf("Starting MiniWeb (kqueue) on %s:%d\n",
		   config.bind_addr, config.port);

	/* -- Listen socket -- */
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) err(1, "socket");

	int on = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	set_nonblock(listen_fd);

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port   = htons(config.port);
	if (inet_pton(AF_INET, config.bind_addr, &sa.sin_addr) != 1)
		errx(1, "Invalid bind address: %s", config.bind_addr);

	if (bind(listen_fd,   (struct sockaddr *)&sa, sizeof(sa)) < 0) err(1, "bind");
	if (listen(listen_fd, LISTEN_BACKLOG) < 0)                      err(1, "listen");

	/* -- kqueue -- */
	kq_fd = kqueue();
	if (kq_fd < 0) err(1, "kqueue");

	/* Register listen socket. EV_CLEAR resets the backlog counter after each
	 * kevent() return so we don't accumulate stale counts. */
	struct kevent chg;
	EV_SET(&chg, listen_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
	if (kevent(kq_fd, &chg, 1, NULL, 0, NULL) < 0)
		err(1, "kevent: add listen");

	/* -- Work queue + worker threads -- */
	queue_init(&wq);

	pthread_t threads[THREAD_POOL_SIZE];
	for (int i = 0; i < config.threads; i++) {
		if (pthread_create(&threads[i], NULL, worker_thread, NULL) != 0)
			err(1, "pthread_create");
	}

	apply_openbsd_security();

	printf("Server started. Workers: %d  MaxConns: %d  Port: %d\n"
	"Press Ctrl+C to stop.\n\n",
	config.threads, config.max_conns, config.port);

	/* -- Main event loop (dispatcher only) -- */
	struct kevent events[MAX_EVENTS];
	time_t last_sweep = time(NULL);

	while (running) {
		struct timespec timeout = {1, 0};   /* wake up at least once/second */
		int n = kevent(kq_fd, NULL, 0, events, MAX_EVENTS, &timeout);

		if (n < 0) {
			if (errno == EINTR) continue;
			perror("kevent");
			break;
		}

		/* Idle timeout sweep once per second */
		time_t now = time(NULL);
		if (now - last_sweep >= 1) {
			sweep_idle_connections();
			last_sweep = now;
		}

		for (int i = 0; i < n; i++) {
			struct kevent *ev = &events[i];

			/* -- New connection -- */
			if ((int)ev->ident == listen_fd) {
				handle_accept();
				continue;
			}

			int fd = (int)ev->ident;
			if (fd < 0 || fd >= MAX_CONNECTIONS)
				continue;

			/* -- Validate udata pointer against generation counter -- */
			connection_t *conn = (connection_t *)ev->udata;

			pthread_mutex_lock(&conn_mutex);
			int stale = (!conn ||
			connections[fd] != conn ||
			conn->gen != conn_gen[fd]);
			pthread_mutex_unlock(&conn_mutex);

			if (stale) {
				/* fd was recycled; the kevent will be removed automatically
				 * when the fd was closed, nothing else to do. */
				continue;
			}

			/* -- EOF or error: close immediately, don't bother queuing -- */
			if (ev->flags & (EV_EOF | EV_ERROR)) {
				close(fd);
				free_connection(fd);
				continue;
			}

			if (ev->filter != EVFILT_READ)
				continue;

			/* -- Dispatch to worker pool --
			 * EV_DISPATCH already auto-disabled the event; the worker will
			 * re-enable it via EV_ENABLE if it needs more data, or close the
			 * fd when done (which removes the kevent automatically). */
			if (queue_push(&wq, conn) < 0) {
				/* Queue full: server overloaded, drop the connection */
				send_error_response(fd, 503, "Server busy");
				close(fd);
				free_connection(fd);
			}
		}
	}

	/* -- Graceful shutdown -- */
	printf("\nShutting down...\n");
	running = 0;
	queue_broadcast_shutdown(&wq);

	for (int i = 0; i < config.threads; i++)
		pthread_join(threads[i], NULL);

	pthread_mutex_lock(&conn_mutex);
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (connections[i]) {
			close(connections[i]->fd);
			free(connections[i]);
			connections[i] = NULL;
		}
	}
	pthread_mutex_unlock(&conn_mutex);

	close(listen_fd);
	close(kq_fd);

	printf("Server stopped.\n");
	return 0;
}
