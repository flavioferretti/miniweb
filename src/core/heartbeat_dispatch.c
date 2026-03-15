#include "heartbeat_internal.h"

/** @brief hb_batch_reset function. */
void
hb_batch_reset(hb_task_batch_t *batch)
{
	batch->count = 0;
}

/** @brief hb_batch_push function. */
void
hb_batch_push(hb_task_batch_t *batch, const struct hb_task *task)
{
	if (batch->count >= HB_MAX_TASKS)
		return;
	batch->tasks[batch->count++] = *task;
}

/** @brief hb_batch_run function. */
void
hb_batch_run(const hb_task_batch_t *batch)
{
	for (int i = 0; i < batch->count; i++)
		batch->tasks[i].cb(batch->tasks[i].ctx);
}
