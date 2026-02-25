
#include <miniweb/net/server.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <miniweb/core/log.h>
#include <miniweb/http/handler.h>
#include <miniweb/net/worker.h>

/** Put a socket in non-blocking mode for dispatcher and worker cooperation. */
static void
set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		flags = 0;
	(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/** Accept and register all pending client sockets for EV_DISPATCH reads. */
static void
handle_accept(miniweb_server_runtime_t *rt)
{
	for (;;) {
		struct sockaddr_in caddr;
		socklen_t clen = sizeof(caddr);
		int cfd = accept(rt->listen_fd, (struct sockaddr *)&caddr, &clen);
		if (cfd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (errno == EINTR)
				continue;
			return;
		}
		set_nonblock(cfd);
		miniweb_connection_t *conn = miniweb_connection_alloc(&rt->pool, cfd,
															  &caddr, rt->config->max_conns);
		if (!conn) {
			http_request_t req = {.fd = cfd,.method = "GET",.url = "/",
				.version = "HTTP/1.1",.keep_alive = 0};
				(void)http_send_error(&req, 503, "Server busy");
				close(cfd);
				continue;
		}
		struct kevent ev;
		EV_SET(&ev, cfd, EVFILT_READ, EV_ADD | EV_DISPATCH, 0, 0, conn);
		if (kevent(rt->kq_fd, &ev, 1, NULL, 0, NULL) < 0) {
			close(cfd);
			miniweb_connection_free(&rt->pool, cfd);
		}
	}
}

/** Close timed-out idle sockets discovered in the periodic sweep pass. */
static void
sweep_idle(miniweb_server_runtime_t *rt)
{
	time_t now = time(NULL);
	for (int fd = 0; fd < MINIWEB_MAX_CONNECTIONS; fd++) {
		miniweb_connection_t *c = rt->pool.connections[fd];
		if (c && (now - c->last_activity) > rt->config->conn_timeout) {
			close(fd);
			miniweb_connection_free(&rt->pool, fd);
		}
	}
}

/** Initialize listen socket, kqueue dispatcher, queue/pool, and worker threads. */
int
miniweb_server_run(miniweb_server_runtime_t *rt)
{
	struct sockaddr_in sa;
	pthread_t threads[MINIWEB_THREAD_POOL_SIZE];
	miniweb_worker_runtime_t worker_rt = {.running = &rt->running,
		.kq_fd = &rt->kq_fd,.config = rt->config,.queue = &rt->queue,
		.pool = &rt->pool};

		rt->running = 1;
		miniweb_work_queue_init(&rt->queue);
		miniweb_connection_pool_init(&rt->pool);
		rt->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (rt->listen_fd < 0)
			return -1;
	int on = 1;
	setsockopt(rt->listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	set_nonblock(rt->listen_fd);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(rt->config->port);
	if (inet_pton(AF_INET, rt->config->bind_addr, &sa.sin_addr) != 1)
		return -1;
	if (bind(rt->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
		listen(rt->listen_fd, MINIWEB_LISTEN_BACKLOG) < 0)
		return -1;
	rt->kq_fd = kqueue();
	if (rt->kq_fd < 0)
		return -1;
	struct kevent chg;
	EV_SET(&chg, rt->listen_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
	if (kevent(rt->kq_fd, &chg, 1, NULL, 0, NULL) < 0)
		return -1;
	for (int i = 0; i < rt->config->threads; i++)
		pthread_create(&threads[i], NULL, miniweb_worker_thread, &worker_rt);

	struct kevent events[MINIWEB_MAX_EVENTS];
	time_t last_sweep = time(NULL);
	while (rt->running) {
		struct timespec timeout = {1, 0};
		int n = kevent(rt->kq_fd, NULL, 0, events, MINIWEB_MAX_EVENTS, &timeout);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (time(NULL) - last_sweep >= 1) {
			sweep_idle(rt);
			last_sweep = time(NULL);
		}
		for (int i = 0; i < n; i++) {
			struct kevent *ev = &events[i];
			if ((int)ev->ident == rt->listen_fd) {
				handle_accept(rt);
				continue;
			}
			int fd = (int)ev->ident;
			miniweb_connection_t *conn = (miniweb_connection_t *)ev->udata;
			if (fd < 0 || fd >= MINIWEB_MAX_CONNECTIONS ||
				miniweb_connection_is_stale(&rt->pool, fd, conn))
				continue;
			if (ev->flags & (EV_EOF | EV_ERROR)) {
				close(fd);
				miniweb_connection_free(&rt->pool, fd);
				continue;
			}
			if (miniweb_work_queue_push(&rt->queue, conn) < 0) {
				close(fd);
				miniweb_connection_free(&rt->pool, fd);
			}
		}
	}
	miniweb_work_queue_broadcast_shutdown(&rt->queue);
	for (int i = 0; i < rt->config->threads; i++)
		pthread_join(threads[i], NULL);
	return 0;
}

/** Request asynchronous server shutdown from signal handler context. */
void
miniweb_server_stop(miniweb_server_runtime_t *rt)
{
	rt->running = 0;
}
