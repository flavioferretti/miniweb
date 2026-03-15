#include "heartbeat_internal.h"

/**
 * @brief hb_batch_reset operation.
 *
 * @details Performs the core hb_batch_reset routine for this module.
 *
 * @param batch Input parameter for hb_batch_reset.
 */
void
hb_batch_reset(hb_task_batch_t *batch)
{
	batch->count = 0;
}

/**
 * @brief hb_batch_push operation.
 *
 * @details Performs the core hb_batch_push routine for this module.
 *
 * @param batch Input parameter for hb_batch_push.
 * @param task Input parameter for hb_batch_push.
 */
void
hb_batch_push(hb_task_batch_t *batch, const struct hb_task *task)
{
	if (batch->count >= HB_MAX_TASKS)
		return;
	batch->tasks[batch->count++] = *task;
}

/**
 * @brief hb_batch_run operation.
 *
 * @details Performs the core hb_batch_run routine for this module.
 *
 * @param batch Input parameter for hb_batch_run.
 */
void
hb_batch_run(const hb_task_batch_t *batch)
{
	for (int i = 0; i < batch->count; i++)
		batch->tasks[i].cb(batch->tasks[i].ctx);
}
