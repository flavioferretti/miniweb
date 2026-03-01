/* heartbeat.c - unique heartbeat logic */
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <miniweb/core/heartbeat.h>

#define HB_MAX_TASKS 32
#define HB_IDLE_WAIT_SEC 3600

typedef struct {
	/** Registered task metadata copied at registration time. */
	struct hb_task task;
	/** Next absolute execution time for this task. */
	time_t next_run;
	/** Collected task counters. */
	struct hb_task_stats stats;
	/** Slot activity flag. */
	int active;
} hb_slot_t;

static hb_slot_t g_hb_slots[HB_MAX_TASKS];
static int g_hb_initialized;
static int g_hb_running;
static int g_hb_stop_requested;
static int g_hb_drain_on_stop;
static pthread_t g_hb_thread;
static pthread_mutex_t g_hb_lock = PTHREAD_MUTEX_INITIALIZER;
//static pthread_cond_t g_hb_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_hb_cond; //bugfix - Heartbeat CLOCK_REALTIME Skew

typedef struct {
	struct hb_task tasks[HB_MAX_TASKS];
	int count;
} hb_task_batch_t;

static void hb_batch_reset(hb_task_batch_t *batch);
static void hb_batch_push(hb_task_batch_t *batch, const struct hb_task *task);
static void hb_batch_run(const hb_task_batch_t *batch);
static void hb_collect_due_tasks(time_t now, time_t *wake_at, hb_task_batch_t *todo);
static void hb_collect_all_active(time_t now, hb_task_batch_t *todo);
static void hb_mark_stopped(void);
static void hb_wait_for_next_tick(time_t now, time_t wake_at);
/**
 * @brief Run the periodic scheduler loop.
 * @param arg Unused thread argument.
 * @return Always returns NULL when the scheduler exits.
 */
static void *heartbeat_thread(void *arg);

static void
hb_batch_reset(hb_task_batch_t *batch)
{
	batch->count = 0;
}

static void
hb_batch_push(hb_task_batch_t *batch, const struct hb_task *task)
{
	if (batch->count >= HB_MAX_TASKS)
		return;
	batch->tasks[batch->count++] = *task;
}

static void
hb_batch_run(const hb_task_batch_t *batch)
{
	for (int i = 0; i < batch->count; i++)
		batch->tasks[i].cb(batch->tasks[i].ctx);
}

static void
hb_collect_due_tasks(time_t now, time_t *wake_at, hb_task_batch_t *todo)
{
	for (int i = 0; i < HB_MAX_TASKS; i++) {
		uint64_t missed;
		time_t period;
		time_t elapsed;

		if (!g_hb_slots[i].active)
			continue;
		if (*wake_at == 0 || g_hb_slots[i].next_run < *wake_at)
			*wake_at = g_hb_slots[i].next_run;
		if (now < g_hb_slots[i].next_run)
			continue;

		period = (time_t)g_hb_slots[i].task.period_sec;
		elapsed = now - g_hb_slots[i].next_run;
		missed = 0;
		if (period > 0 && elapsed > 0)
			missed = (uint64_t)(elapsed / period);

		hb_batch_push(todo, &g_hb_slots[i].task);
		g_hb_slots[i].stats.runs++;
		g_hb_slots[i].stats.overruns += missed;
		g_hb_slots[i].stats.last_run = now;
		g_hb_slots[i].stats.last_error = 0;
		g_hb_slots[i].next_run +=
			(time_t)((missed + 1U) * (uint64_t)g_hb_slots[i].task.period_sec);
	}
}

static void
hb_collect_all_active(time_t now, hb_task_batch_t *todo)
{
	for (int i = 0; i < HB_MAX_TASKS; i++) {
		if (!g_hb_slots[i].active)
			continue;
		hb_batch_push(todo, &g_hb_slots[i].task);
		g_hb_slots[i].stats.runs++;
		g_hb_slots[i].stats.last_run = now;
		g_hb_slots[i].stats.last_error = 0;
	}
}

static void
hb_mark_stopped(void)
{
	g_hb_running = 0;
	g_hb_stop_requested = 0;
	g_hb_drain_on_stop = 0;
}

static void
hb_wait_for_next_tick(time_t now, time_t wake_at)
{
	struct timespec ts;
	struct timespec now_mono;

	clock_gettime(CLOCK_MONOTONIC, &now_mono);
	if (wake_at != 0 && wake_at > now) {
		time_t delta = wake_at - now;
		ts.tv_sec = now_mono.tv_sec + delta;
		ts.tv_nsec = now_mono.tv_nsec;
	} else if (wake_at == 0) {
		ts.tv_sec = now_mono.tv_sec + HB_IDLE_WAIT_SEC;
		ts.tv_nsec = now_mono.tv_nsec;
	} else {
		ts.tv_sec = now_mono.tv_sec;
		ts.tv_nsec = now_mono.tv_nsec;
	}
	(void)pthread_cond_timedwait(&g_hb_cond, &g_hb_lock, &ts);
}

/**
 * @brief Initialize global heartbeat state.
 * @return Always returns 0.
 */
int
heartbeat_init(void)
{
	pthread_mutex_lock(&g_hb_lock);
	if (!g_hb_initialized) {
		memset(g_hb_slots, 0, sizeof(g_hb_slots));

		/* Initialize condition variable with CLOCK_MONOTONIC */
		pthread_condattr_t attr;
		pthread_condattr_init(&attr);
		pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
		pthread_cond_init(&g_hb_cond, &attr);
		pthread_condattr_destroy(&attr);

		g_hb_initialized = 1;
	}
	pthread_mutex_unlock(&g_hb_lock);
	return 0;
}

/**
 * @brief Register a new periodic callback.
 * @param task Task descriptor containing name, period, callback and context.
 * @return HB_REGISTER_INSERTED if inserted, HB_REGISTER_DUPLICATE if the name
 *         already exists, or HB_REGISTER_ERROR on invalid input/full table.
 */
int
heartbeat_register(const struct hb_task *task)
{
	time_t now;

	if (!task || !task->name || !task->cb || task->period_sec == 0)
		return HB_REGISTER_ERROR;

	(void)heartbeat_init();
	now = time(NULL);

	pthread_mutex_lock(&g_hb_lock);
	for (int i = 0; i < HB_MAX_TASKS; i++) {
		if (!g_hb_slots[i].active)
			continue;
		if (strcmp(g_hb_slots[i].task.name, task->name) == 0) {
			pthread_mutex_unlock(&g_hb_lock);
			return HB_REGISTER_DUPLICATE;
		}
	}

	for (int i = 0; i < HB_MAX_TASKS; i++) {
		if (g_hb_slots[i].active)
			continue;
		g_hb_slots[i].task = *task;
		g_hb_slots[i].next_run = now + (time_t)task->initial_delay_sec;
		memset(&g_hb_slots[i].stats, 0, sizeof(g_hb_slots[i].stats));
		g_hb_slots[i].active = 1;
		pthread_cond_signal(&g_hb_cond);
		pthread_mutex_unlock(&g_hb_lock);
		return HB_REGISTER_INSERTED;
	}
	pthread_mutex_unlock(&g_hb_lock);
	return HB_REGISTER_ERROR;
}

/**
 * @brief Unregister a task by name.
 * @param name Registered task name.
 * @return 0 on success, -1 if name is invalid or not found.
 */
int
heartbeat_unregister(const char *name)
{
	if (!name)
		return -1;

	(void)heartbeat_init();
	pthread_mutex_lock(&g_hb_lock);
	for (int i = 0; i < HB_MAX_TASKS; i++) {
		if (!g_hb_slots[i].active)
			continue;
		if (strcmp(g_hb_slots[i].task.name, name) == 0) {
			memset(&g_hb_slots[i], 0, sizeof(g_hb_slots[i]));
			pthread_cond_signal(&g_hb_cond);
			pthread_mutex_unlock(&g_hb_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&g_hb_lock);
	return -1;
}

/**
 * @brief Update period, initial delay and context for a task.
 * @param name Registered task name.
 * @param period_sec New period in seconds.
 * @param initial_delay_sec Delay until the next execution.
 * @param ctx New opaque callback context pointer.
 * @return 0 on success, -1 on invalid input or unknown task.
 */
int
heartbeat_update(const char *name,
				 unsigned int period_sec,
				 unsigned int initial_delay_sec,
				 void *ctx)
{
	time_t now;

	if (!name || period_sec == 0)
		return -1;

	(void)heartbeat_init();
	now = time(NULL);
	pthread_mutex_lock(&g_hb_lock);
	for (int i = 0; i < HB_MAX_TASKS; i++) {
		if (!g_hb_slots[i].active)
			continue;
		if (strcmp(g_hb_slots[i].task.name, name) != 0)
			continue;
		g_hb_slots[i].task.period_sec = period_sec;
		g_hb_slots[i].task.initial_delay_sec = initial_delay_sec;
		g_hb_slots[i].task.ctx = ctx;
		g_hb_slots[i].next_run = now + (time_t)initial_delay_sec;
		pthread_cond_signal(&g_hb_cond);
		pthread_mutex_unlock(&g_hb_lock);
		return 0;
	}
	pthread_mutex_unlock(&g_hb_lock);
	return -1;
}

/**
 * @brief Retrieve current task counters.
 * @param name Registered task name.
 * @param stats_out Receives a copy of the current stats.
 * @return 0 on success, -1 on invalid input or unknown task.
 */
int
heartbeat_get_stats(const char *name, struct hb_task_stats *stats_out)
{
	if (!name || !stats_out)
		return -1;

	(void)heartbeat_init();
	pthread_mutex_lock(&g_hb_lock);
	for (int i = 0; i < HB_MAX_TASKS; i++) {
		if (!g_hb_slots[i].active)
			continue;
		if (strcmp(g_hb_slots[i].task.name, name) != 0)
			continue;
		*stats_out = g_hb_slots[i].stats;
		pthread_mutex_unlock(&g_hb_lock);
		return 0;
	}
	pthread_mutex_unlock(&g_hb_lock);
	return -1;
}

/**
 * @brief Start the heartbeat thread if it is not already running.
 * @return 0 on success, -1 on thread creation failure.
 */
int
heartbeat_start(void)
{
	(void)heartbeat_init();
	pthread_mutex_lock(&g_hb_lock);
	if (g_hb_running) {
		pthread_mutex_unlock(&g_hb_lock);
		return 0;
	}
	g_hb_stop_requested = 0;
	g_hb_drain_on_stop = 0;
	if (pthread_create(&g_hb_thread, NULL, heartbeat_thread, NULL) != 0) {
		pthread_mutex_unlock(&g_hb_lock);
		return -1;
	}
	g_hb_running = 1;
	pthread_mutex_unlock(&g_hb_lock);
	return 0;
}

/**
 * @brief Stop heartbeat and wait for the scheduler thread to exit.
 * @return 0 on success.
 */
int
heartbeat_stop(void)
{
	return heartbeat_shutdown(0);
}

/**
 * @brief Stop heartbeat and optionally drain all active tasks one final time.
 * @param drain If non-zero, execute each active task once before shutdown.
 * @return 0 on success.
 */
int
heartbeat_shutdown(int drain)
{
	int should_join = 0;

	(void)heartbeat_init();
	pthread_mutex_lock(&g_hb_lock);
	if (g_hb_running) {
		g_hb_stop_requested = 1;
		g_hb_drain_on_stop = drain ? 1 : 0;
		pthread_cond_signal(&g_hb_cond);
		should_join = 1;
	}
	pthread_mutex_unlock(&g_hb_lock);

	if (should_join)
		(void)pthread_join(g_hb_thread, NULL);

	return 0;
}

/**
 * @brief Thread entry-point that dispatches due heartbeat tasks.
 * @param arg Unused thread argument.
 * @return Always NULL.
 */
static void *
heartbeat_thread(void *arg)
{
	(void)arg;

	for (;;) {
		time_t now;
		time_t wake_at = 0;
		hb_task_batch_t todo;
		int should_stop;
		int drain;

		hb_batch_reset(&todo);
		pthread_mutex_lock(&g_hb_lock);
		now = time(NULL);
		hb_collect_due_tasks(now, &wake_at, &todo);

		should_stop = g_hb_stop_requested;
		drain = g_hb_drain_on_stop;
		if (todo.count == 0 && !should_stop) {
			hb_wait_for_next_tick(now, wake_at);
			pthread_mutex_unlock(&g_hb_lock);
			continue;
		}
		pthread_mutex_unlock(&g_hb_lock);

		hb_batch_run(&todo);

		if (should_stop) {
			if (drain) {
				hb_task_batch_t final_tasks;
				hb_batch_reset(&final_tasks);
				pthread_mutex_lock(&g_hb_lock);
				now = time(NULL);
				hb_collect_all_active(now, &final_tasks);
				pthread_mutex_unlock(&g_hb_lock);
				hb_batch_run(&final_tasks);
			}
			pthread_mutex_lock(&g_hb_lock);
			hb_mark_stopped();
			pthread_mutex_unlock(&g_hb_lock);
			break;
		}
	}

	return NULL;
}
