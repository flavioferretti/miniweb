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

void metrics_json_append_history(char *buffer, size_t size, MetricSample *history,
				 size_t count);
void metrics_json_append_cpu_stats(char *buffer, size_t size);
void metrics_json_append_memory_stats(char *buffer, size_t size);
void metrics_json_append_load_average(char *buffer, size_t size);
void metrics_json_append_os_info(char *buffer, size_t size);
void metrics_json_append_uptime(char *buffer, size_t size);
void metrics_json_append_disk_info(char *buffer, size_t size);
void metrics_json_append_top_ports(char *buffer, size_t size);

#endif
