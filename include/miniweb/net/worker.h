#ifndef MINIWEB_NET_WORKER_H
#define MINIWEB_NET_WORKER_H

#include <signal.h>
#include <sys/event.h>

#include <miniweb/core/conf.h>
#include <miniweb/net/connection_pool.h>
#include <miniweb/net/work_queue.h>

/** Worker runtime wiring shared by all worker threads. */
typedef struct miniweb_worker_runtime {
	volatile sig_atomic_t *running;
	int *kq_fd;
	miniweb_conf_t *config;
	miniweb_work_queue_t *queue;
	miniweb_connection_pool_t *pool;
} miniweb_worker_runtime_t;

/** Process queued sockets and execute HTTP handlers in worker threads. */
void *miniweb_worker_thread(void *arg);

#endif
