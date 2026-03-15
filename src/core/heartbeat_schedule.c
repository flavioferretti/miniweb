#include <stdint.h>
#include <time.h>

#include "heartbeat_internal.h"

/** @brief hb_collect_due_tasks function. */
void
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

/** @brief hb_collect_all_active function. */
void
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

/** @brief hb_mark_stopped function. */
void
hb_mark_stopped(void)
{
	g_hb_running = 0;
	g_hb_stop_requested = 0;
	g_hb_drain_on_stop = 0;
}

/** @brief hb_wait_for_next_tick function. */
void
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
