#ifndef MINIWEB_MODULES_METRICS_INTERNAL_H
#define MINIWEB_MODULES_METRICS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	int64_t ts;
	float cpu;
	uint64_t cpu_total_ticks;
	uint64_t cpu_idle_ticks;
	uint32_t mem_used;
	uint32_t mem_total;
	uint32_t swap_used;
	uint32_t net_rx;
	uint32_t net_tx;
} MetricSample;

/**
 * @brief Append the history section to a metrics JSON document.
 * @param buffer Destination buffer.
 * @param size Destination buffer size.
 * @param history History samples in chronological order.
 * @param count Number of samples in history.
 */
void metrics_json_append_history(char *buffer, size_t size, MetricSample *history,
				 size_t count);

/**
 * @brief Append current CPU statistics to a metrics JSON document.
 * @param buffer Destination buffer.
 * @param size Destination buffer size.
 */
void metrics_json_append_cpu_stats(char *buffer, size_t size);

/**
 * @brief Append current memory and swap statistics to a metrics JSON document.
 * @param buffer Destination buffer.
 * @param size Destination buffer size.
 */
void metrics_json_append_memory_stats(char *buffer, size_t size);

/**
 * @brief Append system load averages to a metrics JSON document.
 * @param buffer Destination buffer.
 * @param size Destination buffer size.
 */
void metrics_json_append_load_average(char *buffer, size_t size);

/**
 * @brief Append OS identification fields to a metrics JSON document.
 * @param buffer Destination buffer.
 * @param size Destination buffer size.
 */
void metrics_json_append_os_info(char *buffer, size_t size);

/**
 * @brief Append human-readable uptime data to a metrics JSON document.
 * @param buffer Destination buffer.
 * @param size Destination buffer size.
 */
void metrics_json_append_uptime(char *buffer, size_t size);

/**
 * @brief Append mounted filesystem usage data to a metrics JSON document.
 * @param buffer Destination buffer.
 * @param size Destination buffer size.
 */
void metrics_json_append_disk_info(char *buffer, size_t size);

/**
 * @brief Append top-ports data to a metrics JSON document.
 * @param buffer Destination buffer.
 * @param size Destination buffer size.
 */
void metrics_json_append_top_ports(char *buffer, size_t size);

/**
 * @brief Append process-focused metrics sections to a metrics JSON document.
 * @param top_cpu_json Output buffer for top CPU processes.
 * @param top_cpu_json_size Size of top_cpu_json.
 * @param top_mem_json Output buffer for top memory processes.
 * @param top_mem_json_size Size of top_mem_json.
 * @param proc_stats_json Output buffer for aggregate process counts.
 * @param proc_stats_json_size Size of proc_stats_json.
 */
void metrics_process_append_json_sections(char *top_cpu_json,
					  size_t top_cpu_json_size,
					  char *top_mem_json,
					  size_t top_mem_json_size,
					  char *proc_stats_json,
					  size_t proc_stats_json_size);

#endif
