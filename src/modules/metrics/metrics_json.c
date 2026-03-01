#include <miniweb/modules/metrics.h>
#include <miniweb/modules/metrics_internal.h>

#include <stdio.h>

/**
 * @brief Append history samples to a JSON section.
 * @param buffer Destination JSON buffer.
 * @param size Destination buffer size.
 * @param history Chronological metrics samples.
 * @param count Number of samples in @p history.
 */
void
metrics_json_append_history(char *buffer, size_t size, MetricSample *history,
			    size_t count)
{
	char *ptr = buffer;
	int written = snprintf(ptr, size, "\"history\": [");
	if (written < 0 || (size_t)written >= size) {
		if (size > 0)
			buffer[0] = '\0';
		return;
	}
	ptr += written;
	size -= (size_t)written;

	for (size_t i = 0; i < count && size > 0; i++) {
		written = snprintf(
		    ptr, size,
		    "%s{\"ts\": %lld, \"cpu\": %.2f, \"mem_used_mb\": %u, "
		    "\"mem_total_mb\": %u, \"swap_used_mb\": %u, "
		    "\"net_rx\": %u, \"net_tx\": %u}",
		    (i > 0) ? ", " : "", (long long)history[i].ts,
		    history[i].cpu, history[i].mem_used, history[i].mem_total,
		    history[i].swap_used, history[i].net_rx, history[i].net_tx);
		if (written < 0 || (size_t)written >= size)
			break;
		ptr += written;
		size -= (size_t)written;
	}

	if (size > 0)
		snprintf(ptr, size, "]");
}

/**
 * @brief Append CPU usage statistics to a JSON section.
 * @param buffer Destination JSON buffer.
 * @param size Destination buffer size.
 */
void
metrics_json_append_cpu_stats(char *buffer, size_t size)
{
	CpuStats stats;
	if (metrics_get_cpu_stats(&stats) == 0) {
		int used =
		    stats.user + stats.nice + stats.system + stats.interrupt;
		snprintf(buffer, size,
			 "\"cpu\": {"
			 "\"used_pct\": %d,"
			 "\"user_pct\": %d,"
			 "\"nice_pct\": %d,"
			 "\"system_pct\": %d,"
			 "\"interrupt_pct\": %d,"
			 "\"idle_pct\": %d"
			 "}",
			 used, stats.user, stats.nice, stats.system,
			 stats.interrupt, stats.idle);
	} else {
		snprintf(buffer, size, "\"cpu\": null");
	}
}

/**
 * @brief Append memory and swap statistics to a JSON section.
 * @param buffer Destination JSON buffer.
 * @param size Destination buffer size.
 */
void
metrics_json_append_memory_stats(char *buffer, size_t size)
{
	MemoryStats stats;
	if (metrics_get_memory_stats(&stats) == 0) {
		snprintf(buffer, size,
			 "\"memory\": {\"total_mb\": %ld, \"free_mb\": %ld, "
			 "\"active_mb\": %ld, \"inactive_mb\": %ld, "
			 "\"wired_mb\": %ld, "
			 "\"cache_mb\": %ld}, "
			 "\"swap\": {\"total_mb\": %ld, \"used_mb\": %ld}",
			 stats.total_mb, stats.free_mb, stats.active_mb,
			 stats.inactive_mb, stats.wired_mb, stats.cache_mb,
			 stats.swap_total_mb, stats.swap_used_mb);
	} else {
		snprintf(
		    buffer, size,
		    "\"memory\": {\"total_mb\": 0, \"free_mb\": 0, "
		    "\"active_mb\": 0, \"inactive_mb\": 0, \"wired_mb\": 0, "
		    "\"cache_mb\": 0}, "
		    "\"swap\": {\"total_mb\": 0, \"used_mb\": 0}");
	}
}

/**
 * @brief Append system load averages to a JSON section.
 * @param buffer Destination JSON buffer.
 * @param size Destination buffer size.
 */
void
metrics_json_append_load_average(char *buffer, size_t size)
{
	LoadAverage load;
	if (metrics_get_load_average(&load) == 0) {
		snprintf(buffer, size,
			 "\"load\": {\"1min\": %.2f, \"5min\": %.2f, "
			 "\"15min\": %.2f}",
			 load.load_1min, load.load_5min, load.load_15min);
	} else {
		snprintf(
		    buffer, size,
		    "\"load\": {\"1min\": 0.0, \"5min\": 0.0, \"15min\": 0.0}");
	}
}

/**
 * @brief Append operating system information to a JSON section.
 * @param buffer Destination JSON buffer.
 * @param size Destination buffer size.
 */
void
metrics_json_append_os_info(char *buffer, size_t size)
{
	char os_type[64], os_release[64], machine[64];
	if (metrics_get_os_info(os_type, os_release, machine,
				sizeof(os_type)) == 0) {
		snprintf(buffer, size,
			 "\"os\": {\"type\": \"%s\", \"release\": \"%s\", "
			 "\"machine\": \"%s\"}",
			 os_type, os_release, machine);
	} else {
		snprintf(buffer, size,
			 "\"os\": {\"type\": \"Unknown\", \"release\": "
			 "\"Unknown\", \"machine\": \"Unknown\"}");
	}
}

/**
 * @brief Append human-readable uptime data to a JSON section.
 * @param buffer Destination JSON buffer.
 * @param size Destination buffer size.
 */
void
metrics_json_append_uptime(char *buffer, size_t size)
{
	char uptime_str[128];
	if (metrics_get_uptime(uptime_str, sizeof(uptime_str)) == 0)
		snprintf(buffer, size, "\"uptime\": \"%s\"", uptime_str);
	else
		snprintf(buffer, size, "\"uptime\": \"unknown\"");
}

/**
 * @brief Append mounted filesystem usage to a JSON section.
 * @param buffer Destination JSON buffer.
 * @param size Destination buffer size.
 */
void
metrics_json_append_disk_info(char *buffer, size_t size)
{
	DiskInfo disks[16];
	int count = metrics_get_disk_usage(disks, 16);

	char *ptr = buffer;
	int written = 0;

	written = snprintf(ptr, size, "\"disks\": [");
	ptr += written;
	size -= written;

	for (int i = 0; i < count && size > 0; i++) {
		if (i > 0) {
			written = snprintf(ptr, size, ", ");
			ptr += written;
			size -= written;
		}

		written = snprintf(
		    ptr, size,
		    "{\"device\": \"%s\", \"mount\": \"%s\", \"total_mb\": "
		    "%ld, \"used_mb\": %ld, \"percent\": %d}",
		    disks[i].device, disks[i].mount_point, disks[i].total_mb,
		    disks[i].used_mb, disks[i].percent_used);
		ptr += written;
		size -= written;
	}

	snprintf(ptr, size, "]");
}

/**
 * @brief Append top-port information to a JSON section.
 * @param buffer Destination JSON buffer.
 * @param size Destination buffer size.
 */
void
metrics_json_append_top_ports(char *buffer, size_t size)
{
	PortInfo ports[20];
	int count = metrics_get_top_ports(ports, 20);

	char *ptr = buffer;
	int written = 0;

	written = snprintf(ptr, size, "\"top_ports\": [");
	ptr += written;
	size -= written;

	for (int i = 0; i < count && size > 0; i++) {
		if (i > 0) {
			written = snprintf(ptr, size, ", ");
			ptr += written;
			size -= written;
		}

		written = snprintf(ptr, size,
				   "{\"port\": %d, \"protocol\": \"%s\", "
				   "\"connections\": %d, \"state\": \"%s\"}",
				   ports[i].port, ports[i].protocol,
				   ports[i].connection_count, ports[i].state);
		ptr += written;
		size -= written;
	}

	snprintf(ptr, size, "]");
}
