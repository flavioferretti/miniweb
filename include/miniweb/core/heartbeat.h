#ifndef MINIWEB_CORE_HEARTBEAT_H
#define MINIWEB_CORE_HEARTBEAT_H

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

int heartbeat_init(void);
int heartbeat_register(const struct hb_task *task);
int heartbeat_start(void);
void heartbeat_stop(void);

#endif
