#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../include/heartbeat.h"

#define HB_MAX_TASKS 32

typedef struct {
	struct hb_task task;
	time_t next_run;
	int active;
} hb_slot_t;

static hb_slot_t g_hb_slots[HB_MAX_TASKS];
static int g_hb_initialized;
static int g_hb_running;
static pthread_t g_hb_thread;
static pthread_mutex_t g_hb_lock = PTHREAD_MUTEX_INITIALIZER;

static void *heartbeat_thread(void *arg);

int
heartbeat_init(void)
{
	pthread_mutex_lock(&g_hb_lock);
	if (!g_hb_initialized) {
		memset(g_hb_slots, 0, sizeof(g_hb_slots));
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
		return -1;

	(void)heartbeat_init();
	now = time(NULL);

	pthread_mutex_lock(&g_hb_lock);
	for (int i = 0; i < HB_MAX_TASKS; i++) {
		if (!g_hb_slots[i].active)
			continue;
		if (strcmp(g_hb_slots[i].task.name, task->name) == 0) {
			pthread_mutex_unlock(&g_hb_lock);
			return 0;
		}
	}

	for (int i = 0; i < HB_MAX_TASKS; i++) {
		if (g_hb_slots[i].active)
			continue;
		g_hb_slots[i].task = *task;
		g_hb_slots[i].next_run = now + (time_t)task->initial_delay_sec;
		g_hb_slots[i].active = 1;
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
	g_hb_running = 1;
	pthread_mutex_unlock(&g_hb_lock);

	if (pthread_create(&g_hb_thread, NULL, heartbeat_thread, NULL) != 0) {
		pthread_mutex_lock(&g_hb_lock);
		g_hb_running = 0;
		pthread_mutex_unlock(&g_hb_lock);
		return -1;
	}
	pthread_detach(g_hb_thread);
	return 0;
}

void
heartbeat_stop(void)
{
	pthread_mutex_lock(&g_hb_lock);
	g_hb_running = 0;
	pthread_mutex_unlock(&g_hb_lock);
}

static void *
heartbeat_thread(void *arg)
{
	(void)arg;

	for (;;) {
		time_t now;
		struct hb_task todo[HB_MAX_TASKS];
		int todo_count = 0;

		pthread_mutex_lock(&g_hb_lock);
		if (!g_hb_running) {
			pthread_mutex_unlock(&g_hb_lock);
			break;
		}
		now = time(NULL);
		for (int i = 0; i < HB_MAX_TASKS; i++) {
			if (!g_hb_slots[i].active)
				continue;
			if (now < g_hb_slots[i].next_run)
				continue;
			todo[todo_count++] = g_hb_slots[i].task;
			g_hb_slots[i].next_run = now +
				(time_t)g_hb_slots[i].task.period_sec;
		}
		pthread_mutex_unlock(&g_hb_lock);

		for (int i = 0; i < todo_count; i++)
			todo[i].cb(todo[i].ctx);

		sleep(1);
	}
	return NULL;
}
