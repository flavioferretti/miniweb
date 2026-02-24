/* heartbeat.h - central heartbeat api */
#ifndef MINIWEB_CORE_HEARTBEAT_H
#define MINIWEB_CORE_HEARTBEAT_H

#include <stdint.h>
#include <time.h>

/**
 * @brief Heartbeat task callback signature.
 * @param ctx Opaque context pointer registered with the task.
 */
typedef void (*mw_heartbeat_cb_t)(void *ctx);

/**
 * @brief Periodic task descriptor for heartbeat scheduling.
 */
struct hb_task {
	const char *name;
	unsigned int period_sec;
	unsigned int initial_delay_sec;
	mw_heartbeat_cb_t cb;
	void *ctx;
};

/**
 * @brief Per-task scheduler counters.
 */
struct hb_task_stats {
	uint64_t runs;
	uint64_t overruns;
	time_t last_run;
	int last_error;
};

enum hb_register_result {
	HB_REGISTER_ERROR = -1,
	HB_REGISTER_DUPLICATE = 0,
	HB_REGISTER_INSERTED = 1,
};

int heartbeat_init(void);
int heartbeat_register(const struct hb_task *task);
int heartbeat_unregister(const char *name);
int heartbeat_update(const char *name,
	unsigned int period_sec,
	unsigned int initial_delay_sec,
	void *ctx);
int heartbeat_get_stats(const char *name, struct hb_task_stats *stats_out);
int heartbeat_start(void);
int heartbeat_stop(void);
int heartbeat_shutdown(int drain);

#endif
