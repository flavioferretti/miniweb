#ifndef MINIWEB_NET_CONNECTION_POOL_H
#define MINIWEB_NET_CONNECTION_POOL_H

#include <netinet/in.h>
#include <pthread.h>
#include <time.h>

#define MINIWEB_MAX_CONNECTIONS 4096
#define MINIWEB_REQUEST_BUFFER_SIZE 16384

typedef struct miniweb_connection {
	int fd;
	struct sockaddr_in addr;
	char buffer[MINIWEB_REQUEST_BUFFER_SIZE];
	size_t bytes_read;
	time_t created;
	time_t last_activity;
	int requests_served;
	unsigned int gen;
} miniweb_connection_t;

typedef struct miniweb_connection_pool {
	miniweb_connection_t *connections[MINIWEB_MAX_CONNECTIONS];
	unsigned int conn_gen[MINIWEB_MAX_CONNECTIONS];
	pthread_mutex_t lock;
	int active_connections;
	miniweb_connection_t pool[MINIWEB_MAX_CONNECTIONS];
	int free_stack[MINIWEB_MAX_CONNECTIONS];
	int free_top;
} miniweb_connection_pool_t;

/** Initialize the in-memory O(1) connection pool and free-list. */
void miniweb_connection_pool_init(miniweb_connection_pool_t *pool);
/** Allocate a connection slot for an accepted socket. */
miniweb_connection_t *miniweb_connection_alloc(miniweb_connection_pool_t *pool,
    int fd, struct sockaddr_in *addr, int max_conns);
/** Free an fd-backed connection slot and bump generation counter. */
void miniweb_connection_free(miniweb_connection_pool_t *pool, int fd);
/** Validate kevent udata against fd slot and generation counter. */
int miniweb_connection_is_stale(miniweb_connection_pool_t *pool, int fd,
    miniweb_connection_t *conn);

#endif
