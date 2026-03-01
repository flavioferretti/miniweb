#ifndef MINIWEB_CORE_HEARTBEAT_INTERNAL_H
#define MINIWEB_CORE_HEARTBEAT_INTERNAL_H

#include <pthread.h>
#include <time.h>

#include <miniweb/core/heartbeat.h>

#define HB_MAX_TASKS 32
#define HB_IDLE_WAIT_SEC 3600

typedef struct {
	struct hb_task task;
	time_t next_run;
	struct hb_task_stats stats;
	int active;
} hb_slot_t;

typedef struct {
	struct hb_task tasks[HB_MAX_TASKS];
	int count;
} hb_task_batch_t;

extern hb_slot_t g_hb_slots[HB_MAX_TASKS];
extern int g_hb_initialized;
extern int g_hb_running;
extern int g_hb_stop_requested;
extern int g_hb_drain_on_stop;
extern pthread_t g_hb_thread;
extern pthread_mutex_t g_hb_lock;
extern pthread_cond_t g_hb_cond;

void hb_batch_reset(hb_task_batch_t *batch);
void hb_batch_push(hb_task_batch_t *batch, const struct hb_task *task);
void hb_batch_run(const hb_task_batch_t *batch);
void hb_collect_due_tasks(time_t now, time_t *wake_at, hb_task_batch_t *todo);
void hb_collect_all_active(time_t now, hb_task_batch_t *todo);
void hb_mark_stopped(void);
void hb_wait_for_next_tick(time_t now, time_t wake_at);

#endif
