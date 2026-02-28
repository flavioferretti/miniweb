#include <miniweb/http/response_internal.h>

#include <miniweb/core/log.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
	char path[512];
	char *data;
	size_t len;
	time_t mtime;
	time_t atime;
} file_cache_entry_t;

typedef struct {
	char path[512];
	unsigned int hits;
	time_t atime;
} file_cache_candidate_t;

typedef struct {
	file_cache_entry_t entries[FILE_CACHE_SLOTS];
	file_cache_candidate_t candidates[FILE_CACHE_SLOTS * 2];
	pthread_mutex_t lock;
	int cache_insert_tokens;
	time_t cache_insert_window;
	unsigned int window_hits;
	unsigned int window_misses;
	unsigned int window_inserts;
	unsigned int window_throttles;
} file_cache_shard_t;

static file_cache_shard_t file_cache_shards[FILE_CACHE_SHARDS];
static pthread_once_t http_globals_once = PTHREAD_ONCE_INIT;
static int g_http_globals_initialized;


static int
file_cache_shard_index(const char *path)
{
	const unsigned char *p;
	unsigned long hash;

	hash = 1469598103934665603UL;
	for (p = (const unsigned char *)path; *p; p++) {
		hash ^= *p;
		hash *= 1099511628211UL;
	}

	return (int)(hash % FILE_CACHE_SHARDS);
}

static void
file_cache_init_shards(void)
{
	int i;

	for (i = 0; i < FILE_CACHE_SHARDS; i++) {
		pthread_mutex_init(&file_cache_shards[i].lock, NULL);
		file_cache_shards[i].cache_insert_tokens = FILE_CACHE_INSERTS_PER_SEC;
	}
}

static void
http_handler_globals_init(void)
{
	http_response_pool_init_shards();
	file_cache_init_shards();
	g_http_globals_initialized = 1;
}

void
http_handler_globals_init_once(void)
{
	pthread_once(&http_globals_once, http_handler_globals_init);
}

static void
file_cache_refill_budget_locked(file_cache_shard_t *shard, time_t now,
    int shard_idx)
{
	if (shard->cache_insert_window == 0 || now != shard->cache_insert_window) {
		if (shard->cache_insert_window != 0) {
			log_debug("[FILE_CACHE] shard=%d/sec=%ld hits=%u "
			    "misses=%u inserts=%u throttles=%u budget=%d",
			    shard_idx, (long)shard->cache_insert_window,
			    shard->window_hits, shard->window_misses,
			    shard->window_inserts, shard->window_throttles,
			    FILE_CACHE_INSERTS_PER_SEC);
		}
		shard->cache_insert_window = now;
		shard->cache_insert_tokens = FILE_CACHE_INSERTS_PER_SEC;
		shard->window_hits = 0;
		shard->window_misses = 0;
		shard->window_inserts = 0;
		shard->window_throttles = 0;
	}
}

static void
file_cache_evict_stale_locked(file_cache_shard_t *shard, time_t now)
{
	int i;

	for (i = 0; i < FILE_CACHE_SLOTS; i++) {
		if (shard->entries[i].path[0] == '\0')
			continue;
		if ((now - shard->entries[i].atime) > FILE_CACHE_MAX_AGE_SEC) {
			free(shard->entries[i].data);
			memset(&shard->entries[i], 0, sizeof(shard->entries[i]));
		}
	}

	for (i = 0; i < FILE_CACHE_SLOTS * 2; i++) {
		if (shard->candidates[i].path[0] == '\0')
			continue;
		if ((now - shard->candidates[i].atime) > FILE_CACHE_MAX_AGE_SEC)
			memset(&shard->candidates[i], 0, sizeof(shard->candidates[i]));
	}
}

static int
file_cache_admit_locked(file_cache_shard_t *shard, const char *path, time_t now)
{
	time_t oldest;
	int i;
	int slot;

	slot = -1;
	oldest = 0;
	for (i = 0; i < FILE_CACHE_SLOTS * 2; i++) {
		if (shard->candidates[i].path[0] == '\0') {
			slot = i;
			break;
		}
		if (strcmp(shard->candidates[i].path, path) == 0) {
			shard->candidates[i].hits++;
			shard->candidates[i].atime = now;
			return shard->candidates[i].hits >= 2;
		}
		if (slot == -1 || shard->candidates[i].atime < oldest) {
			oldest = shard->candidates[i].atime;
			slot = i;
		}
	}
	if (slot < 0)
		return 0;

	strlcpy(shard->candidates[slot].path, path,
	    sizeof(shard->candidates[slot].path));
	shard->candidates[slot].hits = 1;
	shard->candidates[slot].atime = now;
	return 0;
}

void
http_file_cache_store(const char *path, const struct stat *st, const char *data,
    size_t len)
{
	file_cache_shard_t *shard;
	time_t now;
	time_t oldest;
	int i;
	int shard_idx;
	int slot;

	if (!path || !st || !data || len == 0 || len > FILE_CACHE_MAX_BYTES)
		return;

	http_handler_globals_init_once();
	shard_idx = file_cache_shard_index(path);
	shard = &file_cache_shards[shard_idx];

	pthread_mutex_lock(&shard->lock);
	now = time(NULL);
	file_cache_refill_budget_locked(shard, now, shard_idx);
	file_cache_evict_stale_locked(shard, now);

	if (shard->cache_insert_tokens <= 0) {
		shard->window_throttles++;
		pthread_mutex_unlock(&shard->lock);
		return;
	}
	if (!file_cache_admit_locked(shard, path, now)) {
		pthread_mutex_unlock(&shard->lock);
		return;
	}

	slot = -1;
	oldest = 0;
	for (i = 0; i < FILE_CACHE_SLOTS; i++) {
		if (shard->entries[i].path[0] == '\0') {
			slot = i;
			break;
		}
		if (slot == -1 || shard->entries[i].atime < oldest) {
			oldest = shard->entries[i].atime;
			slot = i;
		}
	}

	if (slot >= 0) {
		shard->cache_insert_tokens--;
		free(shard->entries[slot].data);
		shard->entries[slot].data = malloc(len);
		if (shard->entries[slot].data) {
			memcpy(shard->entries[slot].data, data, len);
			strlcpy(shard->entries[slot].path, path,
			    sizeof(shard->entries[slot].path));
			shard->entries[slot].len = len;
			shard->entries[slot].mtime = st->st_mtime;
			shard->entries[slot].atime = now;
			shard->window_inserts++;
		} else {
			memset(&shard->entries[slot], 0, sizeof(shard->entries[slot]));
		}
	}

	pthread_mutex_unlock(&shard->lock);
}

int
http_file_cache_lookup(const char *path, const struct stat *st, char **out,
    size_t *out_len)
{
	file_cache_shard_t *shard;
	time_t now;
	int found;
	int i;
	int shard_idx;

	if (!path || !st || !out || !out_len || st->st_size <= 0 ||
	    (size_t)st->st_size > FILE_CACHE_MAX_BYTES)
		return 0;

	http_handler_globals_init_once();
	shard_idx = file_cache_shard_index(path);
	shard = &file_cache_shards[shard_idx];
	found = 0;

	pthread_mutex_lock(&shard->lock);
	now = time(NULL);
	file_cache_refill_budget_locked(shard, now, shard_idx);
	file_cache_evict_stale_locked(shard, now);

	for (i = 0; i < FILE_CACHE_SLOTS; i++) {
		if (shard->entries[i].path[0] == '\0')
			continue;
		if (strcmp(shard->entries[i].path, path) != 0)
			continue;
		if (shard->entries[i].mtime != st->st_mtime)
			continue;

		*out = malloc(shard->entries[i].len);
		if (*out) {
			memcpy(*out, shard->entries[i].data, shard->entries[i].len);
			*out_len = shard->entries[i].len;
			shard->entries[i].atime = now;
			shard->window_hits++;
			found = 1;
		}
		break;
	}
	if (!found)
		shard->window_misses++;
	pthread_mutex_unlock(&shard->lock);

	return found;
}

void
http_handler_globals_cleanup(void)
{
	file_cache_shard_t *shard;
	int i;
	int shard_idx;

	if (!g_http_globals_initialized)
		return;

	for (shard_idx = 0; shard_idx < FILE_CACHE_SHARDS; shard_idx++) {
		shard = &file_cache_shards[shard_idx];
		pthread_mutex_lock(&shard->lock);
		for (i = 0; i < FILE_CACHE_SLOTS; i++) {
			free(shard->entries[i].data);
			shard->entries[i].data = NULL;
			memset(shard->entries[i].path, 0,
			    sizeof(shard->entries[i].path));
			shard->entries[i].len = 0;
		}
		pthread_mutex_unlock(&shard->lock);
	}
}
