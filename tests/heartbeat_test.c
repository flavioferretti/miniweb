#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <miniweb/core/heartbeat.h>

struct hb_counter_ctx {
	pthread_mutex_t lock;
	int runs;
};

/** @brief count_cb function. */
static void
count_cb(void *ctx)
{
	struct hb_counter_ctx *counter = (struct hb_counter_ctx *)ctx;
	pthread_mutex_lock(&counter->lock);
	counter->runs++;
	pthread_mutex_unlock(&counter->lock);
}

/** @brief counter_value function. */
static int
counter_value(struct hb_counter_ctx *counter)
{
	int v;
	pthread_mutex_lock(&counter->lock);
	v = counter->runs;
	pthread_mutex_unlock(&counter->lock);
	return v;
}

/** @brief register_worker function. */
static void *
register_worker(void *arg)
{
	struct hb_task *task = (struct hb_task *)arg;
	for (int i = 0; i < 100; i++) {
		(void)heartbeat_register(task);
		usleep(1000);
	}
	return NULL;
}

/** @brief start_stop_worker function. */
static void *
start_stop_worker(void *arg)
{
	(void)arg;
	for (int i = 0; i < 50; i++) {
		assert(heartbeat_start() == 0);
		usleep(1000);
		assert(heartbeat_stop() == 0);
	}
	return NULL;
}

/** @brief main function. */
int
main(void)
{
	struct hb_counter_ctx counter;
	struct hb_task task;
	struct hb_task_stats stats;
	pthread_t reg_thread;
	pthread_t ss_thread;

	memset(&counter, 0, sizeof(counter));
	assert(pthread_mutex_init(&counter.lock, NULL) == 0);

	task.name = "heartbeat-test";
	task.period_sec = 1;
	task.initial_delay_sec = 0;
	task.cb = count_cb;
	task.ctx = &counter;

	assert(heartbeat_init() == 0);
	assert(heartbeat_register(&task) == HB_REGISTER_INSERTED);
	assert(heartbeat_register(&task) == HB_REGISTER_DUPLICATE);
	assert(heartbeat_update(task.name, 1, 1, &counter) == 0);
	assert(heartbeat_update("missing", 1, 0, &counter) == -1);
	assert(heartbeat_get_stats(task.name, &stats) == 0);
	assert(stats.runs == 0);

	assert(heartbeat_start() == 0);
	assert(heartbeat_start() == 0);
	sleep(2);
	assert(counter_value(&counter) >= 1);

	assert(heartbeat_get_stats(task.name, &stats) == 0);
	assert(stats.runs >= 1);
	assert(stats.last_run != 0);

	assert(heartbeat_stop() == 0);
	assert(heartbeat_stop() == 0);

	assert(pthread_create(&reg_thread, NULL, register_worker, &task) == 0);
	assert(pthread_create(&ss_thread, NULL, start_stop_worker, NULL) == 0);
	assert(pthread_join(reg_thread, NULL) == 0);
	assert(pthread_join(ss_thread, NULL) == 0);

	assert(heartbeat_shutdown(1) == 0);
	assert(heartbeat_unregister(task.name) == 0);
	assert(heartbeat_unregister(task.name) == -1);
	assert(heartbeat_get_stats(task.name, &stats) == -1);

	assert(pthread_mutex_destroy(&counter.lock) == 0);
	puts("heartbeat_test: ok");
	return 0;
}
