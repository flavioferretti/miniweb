#include <miniweb/http/response_internal.h>

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	http_response_t items[1024];
	int free_stack[1024];
	int free_top;
	pthread_mutex_t lock;
	int initialized;
} response_pool_t;

static response_pool_t response_pools[RESPONSE_POOL_SHARDS];

/**
 * @brief thread_hash operation.
 *
 * @details Performs the core thread_hash routine for this module.
 *
 * @return Return value produced by thread_hash.
 */
static inline unsigned long
thread_hash(void)
{
	uintptr_t tid;

	tid = (uintptr_t)pthread_self();
	return (unsigned long)(tid ^ (tid >> 7) ^ (tid >> 13));
}

/**
 * @brief response_pool_shard_index operation.
 *
 * @details Performs the core response_pool_shard_index routine for this module.
 *
 * @return Return value produced by response_pool_shard_index.
 */
static int
response_pool_shard_index(void)
{
	return (int)(thread_hash() % RESPONSE_POOL_SHARDS);
}

/**
 * @brief response_pool_init_locked operation.
 *
 * @details Performs the core response_pool_init_locked routine for this module.
 *
 * @param pool Input parameter for response_pool_init_locked.
 */
static void
response_pool_init_locked(response_pool_t *pool)
{
	int i;

	if (pool->initialized)
		return;

	pool->free_top = 1024;
	for (i = 0; i < 1024; i++)
		pool->free_stack[i] = 1023 - i;
	pool->initialized = 1;
}

/**
 * @brief http_response_pool_init_shards operation.
 *
 * @details Performs the core http_response_pool_init_shards routine for this module.
 */
void
http_response_pool_init_shards(void)
{
	int i;

	for (i = 0; i < RESPONSE_POOL_SHARDS; i++)
		pthread_mutex_init(&response_pools[i].lock, NULL);
}

/**
 * @brief http_response_pool_acquire operation.
 *
 * @details Performs the core http_response_pool_acquire routine for this module.
 *
 * @return Return value produced by http_response_pool_acquire.
 */
http_response_t *
http_response_pool_acquire(void)
{
	response_pool_t *pool;
	http_response_t *resp;
	int idx;
	int shard_idx;

	shard_idx = response_pool_shard_index();
	pool = &response_pools[shard_idx];
	resp = NULL;

	pthread_mutex_lock(&pool->lock);
	response_pool_init_locked(pool);
	if (pool->free_top > 0) {
		idx = pool->free_stack[--pool->free_top];
		resp = &pool->items[idx];
		memset(resp, 0, sizeof(*resp));
	}
	pthread_mutex_unlock(&pool->lock);

	if (resp)
		resp->pool_shard_idx = shard_idx;
	return resp;
}

/**
 * @brief http_response_pool_release operation.
 *
 * @details Performs the core http_response_pool_release routine for this module.
 *
 * @param resp Input parameter for http_response_pool_release.
 *
 * @return Return value produced by http_response_pool_release.
 */
int
http_response_pool_release(http_response_t *resp)
{
	response_pool_t *pool;
	ptrdiff_t idx;
	int shard_idx;

	if (!resp)
		return 0;

	shard_idx = resp->pool_shard_idx;
	if (shard_idx < 0 || shard_idx >= RESPONSE_POOL_SHARDS)
		return 0;

	pool = &response_pools[shard_idx];
	if (resp < pool->items || resp >= (pool->items + 1024))
		return 0;

	idx = resp - pool->items;
	pthread_mutex_lock(&pool->lock);
	memset(resp, 0, sizeof(*resp));
	if (pool->free_top < 1024)
		pool->free_stack[pool->free_top++] = (int)idx;
	pthread_mutex_unlock(&pool->lock);
	return 1;
}
