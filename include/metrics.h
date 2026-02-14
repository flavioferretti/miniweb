/* metrics.h - System metrics header */
#ifndef METRICS_H
#define METRICS_H

#include <microhttpd.h>

/* CPU statistics structure */
typedef struct {
	int user;
	int nice;
	int system;
	int interrupt;
	int idle;
} CpuStats;

/* Memory statistics structure */
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

/* Load average structure */
typedef struct {
	double load_1min;
	double load_5min;
	double load_15min;
} LoadAverage;

/* Disk information structure */
typedef struct {
	char device[64];
	char mount_point[256];
	long total_mb;
	long used_mb;
	int percent_used;
} DiskInfo;

/* Port information structure */
typedef struct {
	int port;
	char protocol[16];
	int connection_count;
	char state[16];
} PortInfo;

/* Network interface structure */
typedef struct {
	char name[32];
	char ip_address[64];
	char status[16];
} NetworkInterface;

/* Process information structure */
typedef struct {
	char user[32];
	int pid;
	float cpu_percent;
	float memory_percent;
	int memory_mb;
	char command[256];
} ProcessInfo;

/* Main API functions */
char *get_system_metrics_json(void);
int metrics_handler(void *cls, struct MHD_Connection *connection,
		    const char *url, const char *method, const char *version,
		    const char *upload_data, size_t *upload_data_size,
		    void **con_cls);

/* Metric collection functions */
int metrics_get_cpu_stats(CpuStats *stats);
int metrics_get_memory_stats(MemoryStats *stats);
int metrics_get_load_average(LoadAverage *load);
int metrics_get_os_info(char *type, char *release, char *machine, size_t size);
int metrics_get_uptime(char *uptime_str, size_t size);
int metrics_get_hostname(char *hostname, size_t size);
int metrics_get_disk_usage(DiskInfo *disks, int max_disks);
int metrics_get_top_ports(PortInfo *ports, int max_ports);
int metrics_get_network_interfaces(NetworkInterface *interfaces,
				   int max_interfaces);
int metrics_get_top_cpu_processes(ProcessInfo *processes, int max_processes);
int metrics_get_top_memory_processes(ProcessInfo *processes, int max_processes);
int metrics_get_process_stats(int *total, int *running, int *sleeping,
			      int *zombie);

#endif /* METRICS_H */
