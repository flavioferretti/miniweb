/* metrics_debug.c - Versione con logging verboso per debug */

/*
 * INSTRUMENTED VERSION
 * Questa versione stampa su stderr ogni passo dell'esecuzione
 * per identificare esattamente dove si blocca.
 *
 * Compila e testa:
 *   make clean && make
 *   ./server -v &
 *   wrk http://localhost:9001/api/metrics
 *
 * Monitora stderr per vedere dove si ferma.
 */

#include "config.h"
#include "metrics.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <math.h>
#include <microhttpd.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifdef __OpenBSD__
#include <sys/mount.h>
#include <sys/sched.h>
#include <sys/statvfs.h>
#include <sys/swap.h>
#include <sys/sysctl.h>
#include <uvm/uvmexp.h>
#endif

#define JSON_BUFFER_SIZE 8192
#define MB (1024 * 1024)

#define LOG(...) do { \
if (config_verbose) { \
	fprintf(stderr, "[METRICS] " __VA_ARGS__); \
	fprintf(stderr, "\n"); \
} \
} while (0)

/* CPU stats disabled */
int
metrics_get_cpu_stats(CpuStats *stats)
{
	LOG("CPU stats (disabled)");
	stats->user = 0;
	stats->nice = 0;
	stats->system = 0;
	stats->interrupt = 0;
	stats->idle = 0;
	return -1;
}

int
metrics_get_memory_stats(MemoryStats *stats)
{
	LOG("Memory stats START");
	int mib[2];
	size_t len;

	unsigned long physmem;
	mib[0] = CTL_HW;
	mib[1] = HW_PHYSMEM64;
	len = sizeof(physmem);
	if (sysctl(mib, 2, &physmem, &len, NULL, 0) == -1) {
		LOG("  sysctl HW_PHYSMEM64 FAILED");
		return -1;
	}
	LOG("  physmem OK");

	struct uvmexp uvm;
	mib[0] = CTL_VM;
	mib[1] = VM_UVMEXP;
	len = sizeof(uvm);
	if (sysctl(mib, 2, &uvm, &len, NULL, 0) == -1) {
		LOG("  sysctl VM_UVMEXP FAILED");
		return -1;
	}
	LOG("  uvmexp OK");

	unsigned long pagesize = uvm.pagesize;
	unsigned long free_mem = uvm.free * pagesize;
	unsigned long active_mem = uvm.active * pagesize;
	unsigned long inactive_mem = uvm.inactive * pagesize;
	unsigned long wired_mem = uvm.wired * pagesize;

	stats->total_mb = physmem / MB;
	stats->free_mb = free_mem / MB;
	stats->active_mb = active_mem / MB;
	stats->inactive_mb = inactive_mem / MB;
	stats->wired_mb = wired_mem / MB;
	stats->cache_mb = 0;

	LOG("  Getting swap info...");
	struct swapent *swdev;
	int nswap, rnswap;

	nswap = swapctl(SWAP_NSWAP, NULL, 0);
	LOG("  swapctl SWAP_NSWAP returned %d", nswap);

	if (nswap == 0) {
		stats->swap_total_mb = 0;
		stats->swap_used_mb = 0;
	} else {
		swdev = calloc(nswap, sizeof(*swdev));
		if (swdev == NULL) {
			LOG("  calloc for swap FAILED");
			stats->swap_total_mb = 0;
			stats->swap_used_mb = 0;
		} else {
			rnswap = swapctl(SWAP_STATS, swdev, nswap);
			LOG("  swapctl SWAP_STATS returned %d", rnswap);

			if (rnswap == -1) {
				stats->swap_total_mb = 0;
				stats->swap_used_mb = 0;
			} else {
				unsigned long swap_total = 0;
				unsigned long swap_used = 0;

				for (int i = 0; i < nswap; i++) {
					swap_total += swdev[i].se_nblks;
					swap_used += swdev[i].se_inuse;
				}

				stats->swap_total_mb = (swap_total * 512) / MB;
				stats->swap_used_mb = (swap_used * 512) / MB;
			}
			free(swdev);
		}
	}

	LOG("Memory stats END");
	return 0;
}

int
metrics_get_load_average(LoadAverage *load)
{
	LOG("Load average START");
	double loadavg[3];

	if (getloadavg(loadavg, 3) == -1) {
		LOG("  getloadavg FAILED");
		return -1;
	}

	load->load_1min = loadavg[0];
	load->load_5min = loadavg[1];
	load->load_15min = loadavg[2];

	LOG("Load average END (%.2f, %.2f, %.2f)", loadavg[0], loadavg[1], loadavg[2]);
	return 0;
}

int
metrics_get_os_info(char *type, char *release, char *machine, size_t size)
{
	LOG("OS info START");
	struct utsname uts;

	if (uname(&uts) == -1) {
		LOG("  uname FAILED");
		return -1;
	}

	strlcpy(type, uts.sysname, size);
	strlcpy(release, uts.release, size);
	strlcpy(machine, uts.machine, size);

	LOG("OS info END");
	return 0;
}

int
metrics_get_uptime(char *uptime_str, size_t size)
{
	LOG("Uptime START");
	struct timeval boottime;
	time_t now;
	size_t len = sizeof(boottime);
	int mib[2] = {CTL_KERN, KERN_BOOTTIME};

	if (sysctl(mib, 2, &boottime, &len, NULL, 0) == -1) {
		LOG("  sysctl KERN_BOOTTIME FAILED");
		strlcpy(uptime_str, "unknown", size);
		return -1;
	}

	time(&now);
	long uptime_seconds = difftime(now, boottime.tv_sec);

	long days = uptime_seconds / (60 * 60 * 24);
	long hours = (uptime_seconds % (60 * 60 * 24)) / (60 * 60);
	long minutes = (uptime_seconds % (60 * 60)) / 60;
	long seconds = uptime_seconds % 60;

	if (days > 0) {
		snprintf(uptime_str, size, "%ld days, %ld:%02ld:%02ld", days,
				 hours, minutes, seconds);
	} else {
		snprintf(uptime_str, size, "%ld:%02ld:%02ld", hours, minutes,
				 seconds);
	}

	LOG("Uptime END");
	return 0;
}

int
metrics_get_hostname(char *hostname, size_t size)
{
	LOG("Hostname START");
	int ret = gethostname(hostname, size);
	LOG("Hostname END");
	return ret;
}

int
metrics_get_disk_usage(DiskInfo *disks, int max_disks)
{
	LOG("Disk usage START");
	struct statfs *mntbuf;
	int mntsize, i, count = 0;

	mntsize = getmntinfo(&mntbuf, MNT_NOWAIT);
	LOG("  getmntinfo returned %d filesystems", mntsize);

	if (mntsize == 0) {
		return 0;
	}

	for (i = 0; i < mntsize && count < max_disks; i++) {
		struct statfs *fs = &mntbuf[i];

		if (strcmp(fs->f_fstypename, "tmpfs") == 0 ||
			strcmp(fs->f_fstypename, "procfs") == 0 ||
			strcmp(fs->f_fstypename, "devfs") == 0 ||
			strcmp(fs->f_fstypename, "fdescfs") == 0) {
			continue;
			}

			if (fs->f_blocks == 0) {
				continue;
			}

			DiskInfo *disk = &disks[count];

			strlcpy(disk->device, fs->f_mntfromname, sizeof(disk->device));
			strlcpy(disk->mount_point, fs->f_mntonname,
					sizeof(disk->mount_point));

			unsigned long total = fs->f_blocks * fs->f_bsize;
			unsigned long available = fs->f_bavail * fs->f_bsize;
			unsigned long used = total - available;

			disk->total_mb = total / MB;
			disk->used_mb = used / MB;

			if (disk->total_mb > 0) {
				disk->percent_used =
				(int)((disk->used_mb * 100) / disk->total_mb);
			} else {
				disk->percent_used = 0;
			}

			count++;
	}

	LOG("Disk usage END (found %d disks)", count);
	return count;
}

int
metrics_get_top_ports(PortInfo *ports, int max_ports)
{
	LOG("Top ports START - calling popen(netstat)...");
	fflush(stderr);

	FILE *fp;
	char buffer[512];
	int port_count = 0;

	fp = popen("netstat -an -f inet -p tcp 2>/dev/null | grep LISTEN", "r");
	if (fp == NULL) {
		LOG("  popen FAILED! errno=%d", errno);
		return 0;
	}
	LOG("  popen succeeded");

	while (fgets(buffer, sizeof(buffer), fp) != NULL && port_count < max_ports) {
		char proto[16], recv_q[16], send_q[16];
		char local_addr[128], foreign_addr[128], state[32];

		if (sscanf(buffer, "%15s %15s %15s %127s %127s %31s", proto,
			recv_q, send_q, local_addr, foreign_addr, state) == 6) {

			char *last_dot = strrchr(local_addr, '.');
		if (last_dot != NULL) {
			int port = atoi(last_dot + 1);
			if (port > 0 && port < 65536) {
				ports[port_count].port = port;
				strlcpy(ports[port_count].protocol, proto,
						sizeof(ports[port_count].protocol));
				strlcpy(ports[port_count].state, state,
						sizeof(ports[port_count].state));
				ports[port_count].connection_count = 1;
				port_count++;
			}
		}
			}
	}

	LOG("  Read %d lines, calling pclose...", port_count);
	fflush(stderr);

	pclose(fp);

	LOG("Top ports END (found %d ports)", port_count);
	return port_count;
}

int
metrics_get_network_interfaces(NetworkInterface *interfaces, int max_interfaces)
{
	LOG("Network interfaces START");
	struct ifaddrs *ifaddr, *ifa;
	int count = 0;

	if (getifaddrs(&ifaddr) == -1) {
		LOG("  getifaddrs FAILED");
		return 0;
	}

	for (ifa = ifaddr; ifa != NULL && count < max_interfaces; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL) {
			continue;
		}

		if (ifa->ifa_addr->sa_family != AF_INET) {
			continue;
		}

		int duplicate = 0;
		for (int i = 0; i < count; i++) {
			if (strcmp(interfaces[i].name, ifa->ifa_name) == 0) {
				duplicate = 1;
				break;
			}
		}
		if (duplicate) {
			continue;
		}

		strlcpy(interfaces[count].name, ifa->ifa_name,
				sizeof(interfaces[count].name));

		struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
		inet_ntop(AF_INET, &addr->sin_addr,
				  interfaces[count].ip_address,
			sizeof(interfaces[count].ip_address));

		if (ifa->ifa_flags & IFF_UP) {
			strlcpy(interfaces[count].status, "up",
					sizeof(interfaces[count].status));
		} else {
			strlcpy(interfaces[count].status, "down",
					sizeof(interfaces[count].status));
		}

		count++;
	}

	freeifaddrs(ifaddr);
	LOG("Network interfaces END (found %d)", count);
	return count;
}

int
metrics_get_top_cpu_processes(ProcessInfo *processes, int max_processes)
{
	LOG("Top CPU processes START - calling popen(ps)...");
	fflush(stderr);

	FILE *fp;
	char buffer[512];
	int count = 0;

	fp = popen("ps aux | sort -k3 -r | head -11 | tail -10", "r");
	if (fp == NULL) {
		LOG("  popen FAILED! errno=%d", errno);
		return 0;
	}
	LOG("  popen succeeded");

	while (fgets(buffer, sizeof(buffer), fp) != NULL && count < max_processes) {
		char user[32], pid_str[16], cpu_str[16], mem_str[16],
		vsz_str[16], rss_str[16];
		char tt[16], stat[16], started[16], time[16];
		char command[256];

		if (sscanf(buffer,
			"%31s %15s %15s %15s %15s %15s %15s %15s %15s "
			"%15s %255[^\n]",
			 user, pid_str, cpu_str, mem_str, vsz_str, rss_str,
			 tt, stat, started, time, command) == 11) {

			processes[count].pid = atoi(pid_str);
		processes[count].cpu_percent = atof(cpu_str);
		processes[count].memory_percent = atof(mem_str);
		processes[count].memory_mb = atoi(rss_str) / 1024;

		strlcpy(processes[count].user, user,
				sizeof(processes[count].user));
		strlcpy(processes[count].command, command,
				sizeof(processes[count].command));

		count++;
			 }
	}

	LOG("  Read %d lines, calling pclose...", count);
	fflush(stderr);

	pclose(fp);

	LOG("Top CPU processes END (found %d)", count);
	return count;
}

int
metrics_get_top_memory_processes(ProcessInfo *processes, int max_processes)
{
	LOG("Top memory processes START - calling popen(ps)...");
	fflush(stderr);

	FILE *fp;
	char buffer[512];
	int count = 0;

	fp = popen("ps aux | sort -k4 -r | head -11 | tail -10", "r");
	if (fp == NULL) {
		LOG("  popen FAILED! errno=%d", errno);
		return 0;
	}
	LOG("  popen succeeded");

	while (fgets(buffer, sizeof(buffer), fp) != NULL && count < max_processes) {
		char user[32], pid_str[16], cpu_str[16], mem_str[16],
		vsz_str[16], rss_str[16];
		char tt[16], stat[16], started[16], time[16];
		char command[256];

		if (sscanf(buffer,
			"%31s %15s %15s %15s %15s %15s %15s %15s %15s "
			"%15s %255[^\n]",
			 user, pid_str, cpu_str, mem_str, vsz_str, rss_str,
			 tt, stat, started, time, command) == 11) {

			processes[count].pid = atoi(pid_str);
		processes[count].cpu_percent = atof(cpu_str);
		processes[count].memory_percent = atof(mem_str);
		processes[count].memory_mb = atoi(rss_str) / 1024;

		strlcpy(processes[count].user, user,
				sizeof(processes[count].user));
		strlcpy(processes[count].command, command,
				sizeof(processes[count].command));

		count++;
			 }
	}

	LOG("  Read %d lines, calling pclose...", count);
	fflush(stderr);

	pclose(fp);

	LOG("Top memory processes END (found %d)", count);
	return count;
}

int
metrics_get_process_stats(int *total, int *running, int *sleeping, int *zombie)
{
	LOG("Process stats START - calling popen(ps)...");
	fflush(stderr);

	FILE *fp;
	char buffer[512];
	int t = 0, r = 0, s = 0, z = 0;

	fp = popen("ps aux", "r");
	if (fp == NULL) {
		LOG("  popen FAILED! errno=%d", errno);
		return -1;
	}
	LOG("  popen succeeded");

	if (fgets(buffer, sizeof(buffer), fp) == NULL) {
		LOG("  fgets header FAILED");
		pclose(fp);
		return -1;
	}

	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		char stat[16];
		char user[32], pid_str[16], cpu_str[16], mem_str[16];
		char vsz_str[16], rss_str[16], tt[16];
		char started[16], time[16], command[256];

		if (sscanf(buffer,
			"%31s %15s %15s %15s %15s %15s %15s %15s %15s "
			"%15s %255[^\n]",
			 user, pid_str, cpu_str, mem_str, vsz_str, rss_str,
			 tt, stat, started, time, command) == 11) {

			t++;

		if (strchr(stat, 'R') != NULL) {
			r++;
		} else if (strchr(stat, 'S') != NULL ||
			strchr(stat, 'I') != NULL) {
			s++;
			} else if (strchr(stat, 'Z') != NULL) {
				z++;
			}
			 }
	}

	LOG("  Read %d processes, calling pclose...", t);
	fflush(stderr);

	pclose(fp);

	*total = t;
	*running = r;
	*sleeping = s;
	*zombie = z;

	LOG("Process stats END (total=%d)", t);
	return 0;
}

/* JSON generation functions - minimal logging */

static void append_cpu_stats_json(char *buffer, size_t size)
{
	snprintf(buffer, size, "\"cpu\": null");
}

static void append_memory_stats_json(char *buffer, size_t size)
{
	MemoryStats stats;
	if (metrics_get_memory_stats(&stats) == 0) {
		snprintf(buffer, size,
				 "\"memory\": {\"total_mb\": %ld, \"free_mb\": %ld, "
				 "\"active_mb\": %ld, \"inactive_mb\": %ld, \"wired_mb\": %ld, "
				 "\"cache_mb\": %ld}, "
				 "\"swap\": {\"total_mb\": %ld, \"used_mb\": %ld}",
		   stats.total_mb, stats.free_mb, stats.active_mb,
		   stats.inactive_mb, stats.wired_mb, stats.cache_mb,
		   stats.swap_total_mb, stats.swap_used_mb);
	} else {
		snprintf(buffer, size,
				 "\"memory\": {\"total_mb\": 0, \"free_mb\": 0, "
				 "\"active_mb\": 0, \"inactive_mb\": 0, \"wired_mb\": 0, "
				 "\"cache_mb\": 0}, "
				 "\"swap\": {\"total_mb\": 0, \"used_mb\": 0}");
	}
}

static void append_load_average_json(char *buffer, size_t size)
{
	LoadAverage load;
	if (metrics_get_load_average(&load) == 0) {
		snprintf(buffer, size,
				 "\"load\": {\"1min\": %.2f, \"5min\": %.2f, \"15min\": %.2f}",
		   load.load_1min, load.load_5min, load.load_15min);
	} else {
		snprintf(buffer, size,
				 "\"load\": {\"1min\": 0.0, \"5min\": 0.0, \"15min\": 0.0}");
	}
}

static void append_os_info_json(char *buffer, size_t size)
{
	char os_type[64], os_release[64], machine[64];
	if (metrics_get_os_info(os_type, os_release, machine, sizeof(os_type)) == 0) {
		snprintf(buffer, size,
				 "\"os\": {\"type\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\"}",
		   os_type, os_release, machine);
	} else {
		snprintf(buffer, size,
				 "\"os\": {\"type\": \"Unknown\", \"release\": \"Unknown\", \"machine\": \"Unknown\"}");
	}
}

static void append_uptime_json(char *buffer, size_t size)
{
	char uptime_str[128];
	if (metrics_get_uptime(uptime_str, sizeof(uptime_str)) == 0) {
		snprintf(buffer, size, "\"uptime\": \"%s\"", uptime_str);
	} else {
		snprintf(buffer, size, "\"uptime\": \"unknown\"");
	}
}

static void append_disk_info_json(char *buffer, size_t size)
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
			"{\"device\": \"%s\", \"mount\": \"%s\", \"total_mb\": %ld, \"used_mb\": %ld, \"percent\": %d}",
			disks[i].device, disks[i].mount_point, disks[i].total_mb,
			disks[i].used_mb, disks[i].percent_used);
		ptr += written;
		size -= written;
	}

	snprintf(ptr, size, "]");
}

static void append_top_ports_json(char *buffer, size_t size)
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
						   "{\"port\": %d, \"protocol\": \"%s\", \"connections\": %d, \"state\": \"%s\"}",
					 ports[i].port, ports[i].protocol,
					 ports[i].connection_count, ports[i].state);
		ptr += written;
		size -= written;
	}

	snprintf(ptr, size, "]");
}

static void append_network_interfaces_json(char *buffer, size_t size)
{
	NetworkInterface interfaces[10];
	int count = metrics_get_network_interfaces(interfaces, 10);

	char *ptr = buffer;
	int written = 0;

	written = snprintf(ptr, size, "\"network\": [");
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
			"{\"name\": \"%s\", \"ip\": \"%s\", \"status\": \"%s\"}",
			interfaces[i].name, interfaces[i].ip_address,
			interfaces[i].status);
		ptr += written;
		size -= written;
	}

	snprintf(ptr, size, "]");
}

static void append_top_cpu_processes_json(char *buffer, size_t size)
{
	ProcessInfo processes[10];
	int count = metrics_get_top_cpu_processes(processes, 10);

	char *ptr = buffer;
	int written = 0;

	written = snprintf(ptr, size, "\"top_cpu_processes\": [");
	ptr += written;
	size -= written;

	for (int i = 0; i < count && size > 100; i++) {
		if (i > 0) {
			written = snprintf(ptr, size, ", ");
			ptr += written;
			size -= written;
		}

		written = snprintf(ptr, size,
						   "{\"user\": \"%s\", \"pid\": %d, \"cpu_percent\": %.1f, \"command\": \"%s\"}",
					 processes[i].user, processes[i].pid,
					 processes[i].cpu_percent, processes[i].command);
		ptr += written;
		size -= written;
	}

	snprintf(ptr, size, "]");
}

static void append_top_memory_processes_json(char *buffer, size_t size)
{
	ProcessInfo processes[10];
	int count = metrics_get_top_memory_processes(processes, 10);

	char *ptr = buffer;
	int written = 0;

	written = snprintf(ptr, size, "\"top_memory_processes\": [");
	ptr += written;
	size -= written;

	for (int i = 0; i < count && size > 100; i++) {
		if (i > 0) {
			written = snprintf(ptr, size, ", ");
			ptr += written;
			size -= written;
		}

		written = snprintf(ptr, size,
						   "{\"user\": \"%s\", \"pid\": %d, \"memory_percent\": %.1f, \"memory_mb\": %d, \"command\": \"%s\"}",
					 processes[i].user, processes[i].pid,
					 processes[i].memory_percent,
					 processes[i].memory_mb, processes[i].command);
		ptr += written;
		size -= written;
	}

	snprintf(ptr, size, "]");
}

static void append_process_stats_json(char *buffer, size_t size)
{
	int total, running, sleeping, zombie;

	if (metrics_get_process_stats(&total, &running, &sleeping, &zombie) == 0) {
		snprintf(buffer, size,
				 "\"process_stats\": {\"total\": %d, \"running\": %d, \"sleeping\": %d, \"zombie\": %d}",
		   total, running, sleeping, zombie);
	} else {
		snprintf(buffer, size,
				 "\"process_stats\": {\"total\": 0, \"running\": 0, \"sleeping\": 0, \"zombie\": 0}");
	}
}

char *
get_system_metrics_json(void)
{
	static int call_num = 0;
	call_num++;

	LOG("=== get_system_metrics_json() CALL #%d START ===", call_num);

	static char json[JSON_BUFFER_SIZE];
	char timestamp[64];
	char hostname[256];
	time_t now;

	time(&now);
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));

	if (metrics_get_hostname(hostname, sizeof(hostname)) == -1) {
		strlcpy(hostname, "localhost", sizeof(hostname));
	}

	LOG("Generating JSON sections...");

	char cpu_json[256];
	char memory_json[512];
	char load_json[256];
	char os_json[512];
	char uptime_json[256];
	char disks_json[2048];
	char ports_json[2048];
	char network_json[1024];
	char top_cpu_json[2048];
	char top_mem_json[2048];
	char proc_stats_json[256];

	append_cpu_stats_json(cpu_json, sizeof(cpu_json));
	append_memory_stats_json(memory_json, sizeof(memory_json));
	append_load_average_json(load_json, sizeof(load_json));
	append_os_info_json(os_json, sizeof(os_json));
	append_uptime_json(uptime_json, sizeof(uptime_json));
	append_disk_info_json(disks_json, sizeof(disks_json));

	LOG("About to call append_top_ports_json (POTENTIAL HANG POINT)...");
	append_top_ports_json(ports_json, sizeof(ports_json));
	LOG("append_top_ports_json completed");

	append_network_interfaces_json(network_json, sizeof(network_json));

	LOG("About to call append_top_cpu_processes_json (POTENTIAL HANG POINT)...");
	append_top_cpu_processes_json(top_cpu_json, sizeof(top_cpu_json));
	LOG("append_top_cpu_processes_json completed");

	LOG("About to call append_top_memory_processes_json (POTENTIAL HANG POINT)...");
	append_top_memory_processes_json(top_mem_json, sizeof(top_mem_json));
	LOG("append_top_memory_processes_json completed");

	LOG("About to call append_process_stats_json (POTENTIAL HANG POINT)...");
	append_process_stats_json(proc_stats_json, sizeof(proc_stats_json));
	LOG("append_process_stats_json completed");

	LOG("Building final JSON string...");
	snprintf(json, sizeof(json),
			 "{"
			 "\"timestamp\": \"%s\","
			 "\"hostname\": \"%s\","
			 "%s," "%s," "%s," "%s," "%s," "%s," "%s," "%s," "%s," "%s," "%s"
			 "}",
		  timestamp, hostname, cpu_json, memory_json, load_json, os_json,
		  uptime_json, disks_json, ports_json, network_json,
		  top_cpu_json, top_mem_json, proc_stats_json);

	LOG("=== get_system_metrics_json() CALL #%d END ===", call_num);
	return json;
}

int
metrics_handler(void *cls, struct MHD_Connection *connection, const char *url,
				const char *method, const char *version,
				const char *upload_data, size_t *upload_data_size,
				void **con_cls)
{
	(void)cls; (void)url; (void)method; (void)version;
	(void)upload_data; (void)upload_data_size; (void)con_cls;

	LOG(">>> metrics_handler() called");

	char *json_response = get_system_metrics_json();

	LOG(">>> Creating HTTP response");
	struct MHD_Response *response;
	int ret;

	response = MHD_create_response_from_buffer(
		strlen(json_response), json_response, MHD_RESPMEM_PERSISTENT);

	MHD_add_response_header(response, "Content-Type", "application/json");
	MHD_add_response_header(response, "Cache-Control",
							"no-cache, no-store, must-revalidate");
	MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");

	LOG(">>> Queueing response");
	ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);

	LOG(">>> metrics_handler() returning %d", ret);
	return ret;
}
