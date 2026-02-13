/* metrics.c - System metrics collection */

#include "config.h"
#include "metrics.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <microhttpd.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifdef __OpenBSD__
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/swap.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <uvm/uvmexp.h>
#endif

#define JSON_BUFFER_SIZE 8192
#define MB (1024 * 1024)

#define LOG(...)                                                               \
	do {                                                                   \
		if (config_verbose) {                                            \
			fprintf(stderr, "[METRICS] " __VA_ARGS__);                \
			fprintf(stderr, "\n");                                   \
		}                                                              \
	} while (0)

int
metrics_get_cpu_stats(CpuStats *stats)
{
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
#ifdef __OpenBSD__
	int mib[2];
	size_t len;

	struct uvmexp uvm;
	mib[0] = CTL_VM;
	mib[1] = VM_UVMEXP;
	len = sizeof(uvm);
	if (sysctl(mib, 2, &uvm, &len, NULL, 0) == -1)
		return -1;

	unsigned long pagesize = uvm.pagesize;
	unsigned long long physmem =
	    (unsigned long long)uvm.npages * (unsigned long long)pagesize;
	stats->total_mb = physmem / MB;
	stats->free_mb = (uvm.free * pagesize) / MB;
	stats->active_mb = (uvm.active * pagesize) / MB;
	stats->inactive_mb = (uvm.inactive * pagesize) / MB;
	stats->wired_mb = (uvm.wired * pagesize) / MB;
	stats->cache_mb = 0;

	int nswap = swapctl(SWAP_NSWAP, NULL, 0);
	if (nswap <= 0) {
		stats->swap_total_mb = 0;
		stats->swap_used_mb = 0;
		return 0;
	}
	struct swapent *swdev = calloc(nswap, sizeof(*swdev));
	if (!swdev)
		return 0;
	int rnswap = swapctl(SWAP_STATS, swdev, nswap);
	if (rnswap == -1) {
		free(swdev);
		return 0;
	}
	unsigned long swap_total = 0;
	unsigned long swap_used = 0;
	for (int i = 0; i < nswap; i++) {
		swap_total += swdev[i].se_nblks;
		swap_used += swdev[i].se_inuse;
	}
	free(swdev);
	stats->swap_total_mb = (swap_total * 512) / MB;
	stats->swap_used_mb = (swap_used * 512) / MB;
	return 0;
#else
	memset(stats, 0, sizeof(*stats));
	return -1;
#endif
}

int
metrics_get_load_average(LoadAverage *load)
{
	double loadavg[3];

	if (getloadavg(loadavg, 3) == -1) {
		return -1;
	}

	load->load_1min = loadavg[0];
	load->load_5min = loadavg[1];
	load->load_15min = loadavg[2];

	return 0;
}

int
metrics_get_os_info(char *type, char *release, char *machine, size_t size)
{
	struct utsname uts;

	if (uname(&uts) == -1) {
		return -1;
	}

	strlcpy(type, uts.sysname, size);
	strlcpy(release, uts.release, size);
	strlcpy(machine, uts.machine, size);

	return 0;
}

int
metrics_get_uptime(char *uptime_str, size_t size)
{
#ifdef __OpenBSD__
	struct timeval boottime;
	time_t now;
	size_t len = sizeof(boottime);
	int mib[2] = {CTL_KERN, KERN_BOOTTIME};
	if (sysctl(mib, 2, &boottime, &len, NULL, 0) == -1) {
		strlcpy(uptime_str, "unknown", size);
		return -1;
	}
	time(&now);
	long uptime_seconds = difftime(now, boottime.tv_sec);
	long days = uptime_seconds / (60 * 60 * 24);
	long hours = (uptime_seconds % (60 * 60 * 24)) / (60 * 60);
	long minutes = (uptime_seconds % (60 * 60)) / 60;
	long seconds = uptime_seconds % 60;
	if (days > 0)
		snprintf(uptime_str, size, "%ld days, %ld:%02ld:%02ld", days,
			 hours, minutes, seconds);
	else
		snprintf(uptime_str, size, "%ld:%02ld:%02ld", hours, minutes,
			 seconds);
	return 0;
#else
	strlcpy(uptime_str, "unsupported", size);
	return -1;
#endif
}

int
metrics_get_hostname(char *hostname, size_t size)
{
	return gethostname(hostname, size);
}

int
metrics_get_disk_usage(DiskInfo *disks, int max_disks)
{
#ifdef __OpenBSD__
	struct statfs *mntbuf;
	int mntsize, count = 0;
	mntsize = getmntinfo(&mntbuf, MNT_NOWAIT);
	if (mntsize == 0)
		return 0;
	for (int i = 0; i < mntsize && count < max_disks; i++) {
		struct statfs *fs = &mntbuf[i];
		if (strcmp(fs->f_fstypename, "tmpfs") == 0 ||
		    strcmp(fs->f_fstypename, "procfs") == 0 ||
		    strcmp(fs->f_fstypename, "devfs") == 0 ||
		    strcmp(fs->f_fstypename, "fdescfs") == 0 || fs->f_blocks == 0)
			continue;
		DiskInfo *disk = &disks[count++];
		strlcpy(disk->device, fs->f_mntfromname, sizeof(disk->device));
		strlcpy(disk->mount_point, fs->f_mntonname,
			sizeof(disk->mount_point));
		unsigned long total = fs->f_blocks * fs->f_bsize;
		unsigned long available = fs->f_bavail * fs->f_bsize;
		disk->total_mb = total / MB;
		disk->used_mb = (total - available) / MB;
		disk->percent_used =
			disk->total_mb > 0 ?
			(int)((disk->used_mb * 100) / disk->total_mb) :
			0;
	}
	return count;
#else
	(void)disks;
	(void)max_disks;
	return 0;
#endif
}

int
metrics_get_top_ports(PortInfo *ports, int max_ports)
{
	(void)ports;
	(void)max_ports;
	return 0;
}

int
metrics_get_network_interfaces(NetworkInterface *interfaces, int max_interfaces)
{
	struct ifaddrs *ifaddr, *ifa;
	int count = 0;

	if (getifaddrs(&ifaddr) == -1)
		return 0;

	for (ifa = ifaddr; ifa != NULL && count < max_interfaces;
	     ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL)
			continue;

		if (ifa->ifa_addr->sa_family != AF_INET)
			continue;

		int duplicate = 0;
		for (int i = 0; i < count; i++) {
			if (strcmp(interfaces[i].name, ifa->ifa_name) == 0) {
				duplicate = 1;
				break;
			}
		}
		if (duplicate)
			continue;

		strlcpy(interfaces[count].name, ifa->ifa_name,
			sizeof(interfaces[count].name));

		struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
		inet_ntop(AF_INET, &addr->sin_addr, interfaces[count].ip_address,
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
	return count;
}

int
metrics_get_top_cpu_processes(ProcessInfo *processes, int max_processes)
{
#ifdef __OpenBSD__
	/*
	 * KERN_PROC sysctl queries can be blocked when pledge is active,
	 * causing an abort on /api/metrics. Keep endpoint stable by
	 * gracefully disabling per-process rankings in that mode.
	 */
	(void)processes;
	(void)max_processes;
	return 0;

	/*
	if (processes == NULL || max_processes <= 0)
		return 0;

	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
	size_t len = 0;

	if (sysctl(mib, 4, NULL, &len, NULL, 0) == -1 || len == 0)
		return 0;

	struct kinfo_proc *procs = calloc(1, len);
	if (procs == NULL)
		return 0;

	if (sysctl(mib, 4, procs, &len, NULL, 0) == -1) {
		free(procs);
		return 0;
	}

	int nprocs = (int)(len / sizeof(struct kinfo_proc));
	int count = 0;

	for (int i = 0; i < nprocs; i++) {
		float cpu_percent = (100.0f * (float)procs[i].p_pctcpu) / FSCALE;
		if (cpu_percent <= 0.0f)
			continue;

		ProcessInfo proc = {0};
		proc.pid = procs[i].p_pid;
		proc.cpu_percent = cpu_percent;
		strlcpy(proc.command, procs[i].p_comm, sizeof(proc.command));

		struct passwd *pwd = getpwuid(procs[i].p_uid);
		if (pwd != NULL)
			strlcpy(proc.user, pwd->pw_name, sizeof(proc.user));
		else
			strlcpy(proc.user, "unknown", sizeof(proc.user));

		int insert = count;
		if (insert > max_processes - 1)
			insert = max_processes - 1;

		while (insert > 0 && processes[insert - 1].cpu_percent < proc.cpu_percent) {
			if (insert < max_processes)
				processes[insert] = processes[insert - 1];
			insert--;
		}

		if (insert < max_processes) {
			processes[insert] = proc;
			if (count < max_processes)
				count++;
		}
	}

	free(procs);
	return count;
	*/
#else
	(void)processes;
	(void)max_processes;
	return 0;
#endif
}

int
metrics_get_top_memory_processes(ProcessInfo *processes, int max_processes)
{
#ifdef __OpenBSD__
	/* See note in metrics_get_top_cpu_processes(). */
	(void)processes;
	(void)max_processes;
	return 0;

	/*
	if (processes == NULL || max_processes <= 0)
		return 0;

	int vmib[2] = {CTL_VM, VM_UVMEXP};
	struct uvmexp uvm;
	size_t memlen = sizeof(uvm);
	if (sysctl(vmib, 2, &uvm, &memlen, NULL, 0) == -1 || uvm.pagesize == 0 ||
	    uvm.npages == 0)
		return 0;

	unsigned long long physmem =
	    (unsigned long long)uvm.npages * (unsigned long long)uvm.pagesize;

	int pmib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
	size_t len = 0;
	if (sysctl(pmib, 4, NULL, &len, NULL, 0) == -1 || len == 0)
		return 0;

	struct kinfo_proc *procs = calloc(1, len);
	if (procs == NULL)
		return 0;

	if (sysctl(pmib, 4, procs, &len, NULL, 0) == -1) {
		free(procs);
		return 0;
	}

	long page_size = getpagesize();
	int nprocs = (int)(len / sizeof(struct kinfo_proc));
	int count = 0;

	for (int i = 0; i < nprocs; i++) {
		unsigned long long rss_bytes =
			(unsigned long long)procs[i].p_vm_rssize * (unsigned long long)page_size;
		if (rss_bytes == 0)
			continue;

		ProcessInfo proc = {0};
		proc.pid = procs[i].p_pid;
		proc.memory_mb = (int)(rss_bytes / MB);
		proc.memory_percent = (100.0f * (float)rss_bytes) / (float)physmem;
		strlcpy(proc.command, procs[i].p_comm, sizeof(proc.command));

		struct passwd *pwd = getpwuid(procs[i].p_uid);
		if (pwd != NULL)
			strlcpy(proc.user, pwd->pw_name, sizeof(proc.user));
		else
			strlcpy(proc.user, "unknown", sizeof(proc.user));

		int insert = count;
		if (insert > max_processes - 1)
			insert = max_processes - 1;

		while (insert > 0 && processes[insert - 1].memory_mb < proc.memory_mb) {
			if (insert < max_processes)
				processes[insert] = processes[insert - 1];
			insert--;
		}

		if (insert < max_processes) {
			processes[insert] = proc;
			if (count < max_processes)
				count++;
		}
	}

	free(procs);
	return count;
	*/
#else
	(void)processes;
	(void)max_processes;
	return 0;
#endif
}

int
metrics_get_process_stats(int *total, int *running, int *sleeping, int *zombie)
{
#ifdef __OpenBSD__
	/* See note in metrics_get_top_cpu_processes(). */
	*total = 0;
	*running = 0;
	*sleeping = 0;
	*zombie = 0;
	return 0;

	/*
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
	size_t len = 0;

	if (sysctl(mib, 4, NULL, &len, NULL, 0) == -1)
		return -1;

	if (len == 0) {
		*total = 0;
		*running = 0;
		*sleeping = 0;
		*zombie = 0;
		return 0;
	}

	struct kinfo_proc *procs = calloc(1, len);
	if (procs == NULL)
		return -1;

	if (sysctl(mib, 4, procs, &len, NULL, 0) == -1) {
		free(procs);
		return -1;
	}

	int nprocs = (int)(len / sizeof(struct kinfo_proc));
	*total = nprocs;
	*running = 0;
	*sleeping = 0;
	*zombie = 0;

	for (int i = 0; i < nprocs; i++) {
		switch (procs[i].p_stat) {
		case SRUN:
		case SIDL:
			(*running)++;
			break;
		case SSLEEP:
		case SSTOP:
			(*sleeping)++;
			break;
		case SZOMB:
			(*zombie)++;
			break;
		default:
			break;
		}
	}

	free(procs);
	return 0;
	*/
#else
	*total = 0;
	*running = 0;
	*sleeping = 0;
	*zombie = 0;
	return -1;
#endif
}

static void
append_cpu_stats_json(char *buffer, size_t size)
{
	snprintf(buffer, size, "\"cpu\": null");
}

static void
append_memory_stats_json(char *buffer, size_t size)
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

static void
append_load_average_json(char *buffer, size_t size)
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

static void
append_os_info_json(char *buffer, size_t size)
{
	char os_type[64], os_release[64], machine[64];
	if (metrics_get_os_info(os_type, os_release, machine, sizeof(os_type)) ==
	    0) {
		snprintf(buffer, size,
			 "\"os\": {\"type\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\"}",
			 os_type, os_release, machine);
	} else {
		snprintf(buffer, size,
			 "\"os\": {\"type\": \"Unknown\", \"release\": \"Unknown\", \"machine\": \"Unknown\"}");
	}
}

static void
append_uptime_json(char *buffer, size_t size)
{
	char uptime_str[128];
	if (metrics_get_uptime(uptime_str, sizeof(uptime_str)) == 0)
		snprintf(buffer, size, "\"uptime\": \"%s\"", uptime_str);
	else
		snprintf(buffer, size, "\"uptime\": \"unknown\"");
}

static void
append_disk_info_json(char *buffer, size_t size)
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

static void
append_top_ports_json(char *buffer, size_t size)
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

		written = snprintf(
			ptr, size,
			"{\"port\": %d, \"protocol\": \"%s\", \"connections\": %d, \"state\": \"%s\"}",
			ports[i].port, ports[i].protocol, ports[i].connection_count,
			ports[i].state);
		ptr += written;
		size -= written;
	}

	snprintf(ptr, size, "]");
}

static void
append_network_interfaces_json(char *buffer, size_t size)
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

static void
append_top_cpu_processes_json(char *buffer, size_t size)
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

		written = snprintf(
			ptr, size,
			"{\"user\": \"%s\", \"pid\": %d, \"cpu_percent\": %.1f, \"command\": \"%s\"}",
			processes[i].user, processes[i].pid,
			processes[i].cpu_percent, processes[i].command);
		ptr += written;
		size -= written;
	}

	snprintf(ptr, size, "]");
}

static void
append_top_memory_processes_json(char *buffer, size_t size)
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

		written = snprintf(
			ptr, size,
			"{\"user\": \"%s\", \"pid\": %d, \"memory_percent\": %.1f, \"memory_mb\": %d, \"command\": \"%s\"}",
			processes[i].user, processes[i].pid,
			processes[i].memory_percent, processes[i].memory_mb,
			processes[i].command);
		ptr += written;
		size -= written;
	}

	snprintf(ptr, size, "]");
}

static void
append_process_stats_json(char *buffer, size_t size)
{
	int total, running, sleeping, zombie;

	if (metrics_get_process_stats(&total, &running, &sleeping, &zombie) ==
	    0) {
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
	static char json[JSON_BUFFER_SIZE];
	char timestamp[64];
	char hostname[256];
	time_t now;

	time(&now);
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
		 localtime(&now));

	if (metrics_get_hostname(hostname, sizeof(hostname)) == -1)
		strlcpy(hostname, "localhost", sizeof(hostname));

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
	append_top_ports_json(ports_json, sizeof(ports_json));
	append_network_interfaces_json(network_json, sizeof(network_json));
	append_top_cpu_processes_json(top_cpu_json, sizeof(top_cpu_json));
	append_top_memory_processes_json(top_mem_json, sizeof(top_mem_json));
	append_process_stats_json(proc_stats_json, sizeof(proc_stats_json));

	snprintf(json, sizeof(json),
		 "{"
		 "\"timestamp\": \"%s\"," \
		 "\"hostname\": \"%s\"," \
		 "%s," "%s," "%s," "%s," "%s," "%s," "%s," "%s," "%s," "%s," "%s"
		 "}",
		 timestamp, hostname, cpu_json, memory_json, load_json, os_json,
		 uptime_json, disks_json, ports_json, network_json, top_cpu_json,
		 top_mem_json, proc_stats_json);

	return json;
}

int
metrics_handler(void *cls, struct MHD_Connection *connection, const char *url,
		const char *method, const char *version,
		const char *upload_data, size_t *upload_data_size,
		void **con_cls)
{
	(void)cls;
	(void)url;
	(void)method;
	(void)version;
	(void)upload_data;
	(void)upload_data_size;
	(void)con_cls;

	char *json_response = get_system_metrics_json();

	struct MHD_Response *response;
	int ret;

	response = MHD_create_response_from_buffer(strlen(json_response),
		json_response, MHD_RESPMEM_PERSISTENT);

	MHD_add_response_header(response, "Content-Type", "application/json");
	MHD_add_response_header(response, "Cache-Control",
				"no-cache, no-store, must-revalidate");
	MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");

	ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);

	return ret;
}
