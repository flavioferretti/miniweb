/* heartbeat.c - unique heartbeat logic */
#include <pthread.h>
#include <string.h>
#include <time.h>

#include <miniweb/core/heartbeat.h>

#include "heartbeat_internal.h"

hb_slot_t g_hb_slots[HB_MAX_TASKS];
int g_hb_initialized;
int g_hb_running;
int g_hb_stop_requested;
int g_hb_drain_on_stop;
pthread_t g_hb_thread;
pthread_mutex_t g_hb_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_hb_cond;

static void *heartbeat_thread(void *arg);

int
heartbeat_init(void)
{
	pthread_mutex_lock(&g_hb_lock);
	if (!g_hb_initialized) {
		memset(g_hb_slots, 0, sizeof(g_hb_slots));

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

int
heartbeat_stop(void)
{
	return heartbeat_shutdown(0);
}

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
