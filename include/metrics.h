#ifndef METRICS_H
#define METRICS_H

#include "http_handler.h"

/* --- Data Structures --- */

typedef struct {
	int user;
	int nice;
	int system;
	int interrupt;
	int idle;
} CpuStats;

typedef struct {
	long total_mb;
	long free_mb;
	long active_mb;
	long inactive_mb;
	long wired_mb;
	long cache_mb;
	long swap_total_mb;
	long swap_used_mb;
} MemoryStats;

typedef struct {
	double load_1min;
	double load_5min;
	double load_15min;
} LoadAverage;

typedef struct {
	char device[64];
	char mount_point[256];
	long total_mb;
	long used_mb;
	int percent_used;
} DiskInfo;

typedef struct {
	int port;
	char protocol[16];
	int connection_count;
	char state[16];
} PortInfo;

typedef struct {
	char name[32];
	char ip_address[64];
	char status[16];
} NetworkInterface;

typedef struct {
	char user[32];
	int pid;
	float cpu_percent;
	float memory_percent;
	int memory_mb;
	char command[256];
} ProcessInfo;

/* --- Main Functions --- */

/**
 * Build a full JSON payload with host and runtime metrics.
 *
 * Workflow: collects each metrics category, formats each section as JSON,
 * and merges everything into one heap-allocated string.
 *
 * @return Newly allocated JSON string on success, or NULL on allocation/collection failure.
 */
char *get_system_metrics_json(void);

/**
 * HTTP endpoint handler for `/api/metrics`.
 *
 * Workflow: generates the metrics JSON, prepares a JSON HTTP response,
 * applies CORS header, and sends the response to the client.
 *
 * @param req Request context used to send the response.
 * @return 0 or a positive value depending on the HTTP layer result; negative on failure.
 */
int metrics_handler(http_request_t *req);

/* --- Collection Helpers --- */

/**
 * Collect aggregate CPU usage percentages.
 *
 * @param stats Output structure filled with user/nice/system/interrupt/idle percentages.
 * @return 0 on success, -1 on error or unsupported platform.
 */
int metrics_get_cpu_stats(CpuStats *stats);

/**
 * Collect memory and swap usage values (in MB).
 *
 * @param stats Output structure receiving RAM and swap metrics.
 * @return 0 on success, -1 on error or unsupported platform.
 */
int metrics_get_memory_stats(MemoryStats *stats);

/**
 * Collect 1/5/15-minute system load averages.
 *
 * @param load Output structure receiving load averages.
 * @return 0 on success, -1 on error.
 */
int metrics_get_load_average(LoadAverage *load);

/**
 * Collect basic operating system information.
 *
 * @param type Output buffer for OS type.
 * @param release Output buffer for OS release.
 * @param machine Output buffer for machine architecture.
 * @param size Shared buffer size for all output buffers.
 * @return 0 on success, -1 on error.
 */
int metrics_get_os_info(char *type, char *release, char *machine, size_t size);

/**
 * Format system uptime as a human-readable string.
 *
 * @param uptime_str Output buffer receiving uptime text.
 * @param size Size of uptime_str.
 * @return 0 on success, -1 on error or unsupported platform.
 */
int metrics_get_uptime(char *uptime_str, size_t size);

/**
 * Read the local hostname.
 *
 * @param hostname Output buffer receiving hostname.
 * @param size Size of hostname buffer.
 * @return 0 on success, -1 on error.
 */
int metrics_get_hostname(char *hostname, size_t size);

/**
 * Collect mounted filesystem usage information.
 *
 * @param disks Output array populated with disk stats.
 * @param max_disks Maximum number of entries that can be written into disks.
 * @return Number of entries written.
 */
int metrics_get_disk_usage(DiskInfo *disks, int max_disks);

/**
 * Collect top-opened ports information.
 *
 * @param ports Output array for port data.
 * @param max_ports Maximum number of entries writable in ports.
 * @return Number of entries written.
 */
int metrics_get_top_ports(PortInfo *ports, int max_ports);

/**
 * Collect active IPv4 network interfaces.
 *
 * @param interfaces Output array for network interface data.
 * @param max_interfaces Maximum number of entries writable in interfaces.
 * @return Number of entries written.
 */
int metrics_get_network_interfaces(NetworkInterface *interfaces, int max_interfaces);

/**
 * Collect top processes sorted by CPU usage.
 *
 * @param processes Output array receiving process rows.
 * @param max_processes Maximum number of processes to return.
 * @return Number of entries written.
 */
int metrics_get_top_cpu_processes(ProcessInfo *processes, int max_processes);

#endif /* METRICS_H */
