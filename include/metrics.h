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

char *get_system_metrics_json(void);
int metrics_handler(http_request_t *req);

/* --- Collection Helpers --- */
int metrics_get_cpu_stats(CpuStats *stats);
int metrics_get_memory_stats(MemoryStats *stats);
int metrics_get_load_average(LoadAverage *load);
int metrics_get_os_info(char *type, char *release, char *machine, size_t size);
int metrics_get_uptime(char *uptime_str, size_t size);
int metrics_get_hostname(char *hostname, size_t size);
int metrics_get_disk_usage(DiskInfo *disks, int max_disks);
int metrics_get_top_ports(PortInfo *ports, int max_ports);
int metrics_get_network_interfaces(NetworkInterface *interfaces, int max_interfaces);
int metrics_get_top_cpu_processes(ProcessInfo *processes, int max_processes);

#endif /* METRICS_H */
