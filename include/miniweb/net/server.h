#ifndef MINIWEB_NET_SERVER_H
#define MINIWEB_NET_SERVER_H

#include <signal.h>

#include <miniweb/core/conf.h>
#include <miniweb/net/connection_pool.h>
#include <miniweb/net/work_queue.h>

#define MINIWEB_MAX_EVENTS 256
#define MINIWEB_THREAD_POOL_SIZE 32
#define MINIWEB_LISTEN_BACKLOG 1024

/** Runtime server object containing dispatcher, queue, and pool resources. */
typedef struct miniweb_server_runtime {
	volatile sig_atomic_t running;  /* Changed from plain int */
	int kq_fd;
	int listen_fd;
	int spare_fd;
	miniweb_conf_t *config;
	miniweb_work_queue_t queue;
	miniweb_connection_pool_t pool;
} miniweb_server_runtime_t;

/** Initialize listen socket, kqueue dispatcher, queue/pool, and worker threads. */
int miniweb_server_run(miniweb_server_runtime_t *rt);

/** Request asynchronous server shutdown from signal handler context. */
void miniweb_server_stop(miniweb_server_runtime_t *rt);

#endif
