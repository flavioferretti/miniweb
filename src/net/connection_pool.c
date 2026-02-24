#include <miniweb/net/connection_pool.h>

#include <string.h>

/** Initialize the in-memory O(1) connection pool and free-list. */
void
miniweb_connection_pool_init(miniweb_connection_pool_t *pool)
{
	memset(pool, 0, sizeof(*pool));
	pthread_mutex_init(&pool->lock, NULL);
	pool->free_top = MINIWEB_MAX_CONNECTIONS;
	for (int i = 0; i < MINIWEB_MAX_CONNECTIONS; i++)
		pool->free_stack[i] = MINIWEB_MAX_CONNECTIONS - 1 - i;
}

/** Allocate a connection slot for an accepted socket. */
miniweb_connection_t *
miniweb_connection_alloc(miniweb_connection_pool_t *pool, int fd,
    struct sockaddr_in *addr, int max_conns)
{
	if (fd < 0 || fd >= MINIWEB_MAX_CONNECTIONS)
		return NULL;
	pthread_mutex_lock(&pool->lock);
	if (pool->active_connections >= max_conns || pool->connections[fd] != NULL ||
	    pool->free_top <= 0) {
		pthread_mutex_unlock(&pool->lock);
		return NULL;
	}
	int slot = pool->free_stack[--pool->free_top];
	miniweb_connection_t *conn = &pool->pool[slot];
	memset(conn, 0, sizeof(*conn));
	conn->fd = fd;
	conn->created = time(NULL);
	conn->last_activity = conn->created;
	conn->gen = pool->conn_gen[fd];
	if (addr)
		memcpy(&conn->addr, addr, sizeof(*addr));
	pool->connections[fd] = conn;
	pool->active_connections++;
	pthread_mutex_unlock(&pool->lock);
	return conn;
}

/** Free an fd-backed connection slot and bump generation counter. */
void
miniweb_connection_free(miniweb_connection_pool_t *pool, int fd)
{
	if (fd < 0 || fd >= MINIWEB_MAX_CONNECTIONS)
		return;
	pthread_mutex_lock(&pool->lock);
	if (pool->connections[fd]) {
		miniweb_connection_t *conn = pool->connections[fd];
		int pool_idx = (int)(conn - pool->pool);
		if (pool_idx >= 0 && pool_idx < MINIWEB_MAX_CONNECTIONS) {
			memset(conn, 0, sizeof(*conn));
			if (pool->free_top < MINIWEB_MAX_CONNECTIONS)
				pool->free_stack[pool->free_top++] = pool_idx;
		}
		pool->connections[fd] = NULL;
		pool->conn_gen[fd]++;
		if (pool->active_connections > 0)
			pool->active_connections--;
	}
	pthread_mutex_unlock(&pool->lock);
}

/** Validate kevent udata against fd slot and generation counter. */
int
miniweb_connection_is_stale(miniweb_connection_pool_t *pool, int fd,
    miniweb_connection_t *conn)
{
	int stale;
	pthread_mutex_lock(&pool->lock);
	stale = (!conn || pool->connections[fd] != conn ||
	    conn->gen != pool->conn_gen[fd]);
	pthread_mutex_unlock(&pool->lock);
	return stale;
}
