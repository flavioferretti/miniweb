/* metrics.c - System metrics for OpenBSD with libmicrohttpd */

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

/* OpenBSD specific headers */
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

/* ===== SYSTEM FUNCTIONS IMPLEMENTATION ===== */

/* Get CPU statistics using sysctl cp_time with delta sampling */
int
metrics_get_cpu_stats(CpuStats *stats)
{
	int mib[2];
	long cp_time1[CPUSTATES], cp_time2[CPUSTATES];
	long delta[CPUSTATES];
	size_t len = sizeof(cp_time1);

	mib[0] = CTL_KERN;
	mib[1] = KERN_CPTIME;

	/* First sample */
	if (sysctl(mib, 2, &cp_time1, &len, NULL, 0) == -1) {
		return -1;
	}

	/* Sleep for a short interval to get meaningful delta */
	usleep(100000); /* 100ms */

	/* Second sample */
	len = sizeof(cp_time2);
	if (sysctl(mib, 2, &cp_time2, &len, NULL, 0) == -1) {
		return -1;
	}

	/* Calculate deltas */
	long total_delta = 0;
	for (int i = 0; i < CPUSTATES; i++) {
		delta[i] = cp_time2[i] - cp_time1[i];
		total_delta += delta[i];
	}

	if (total_delta == 0) {
		/* No time elapsed, return idle */
		stats->user = 0;
		stats->nice = 0;
		stats->system = 0;
		stats->interrupt = 0;
		stats->idle = 100;
		return 0;
	}

	/* Convert deltas to percentages */
	stats->user = (int)((delta[CP_USER] * 100) / total_delta);
	stats->nice = (int)((delta[CP_NICE] * 100) / total_delta);
	stats->system = (int)((delta[CP_SYS] * 100) / total_delta);
	stats->interrupt = (int)((delta[CP_INTR] * 100) / total_delta);
	stats->idle = (int)((delta[CP_IDLE] * 100) / total_delta);

	return 0;
}

/* Get memory and swap statistics using sysctl */
int
metrics_get_memory_stats(MemoryStats *stats)
{
	int mib[2];
	size_t len;

	/* Get physical memory total */
	unsigned long physmem;
	mib[0] = CTL_HW;
	mib[1] = HW_PHYSMEM64;
	len = sizeof(physmem);
	if (sysctl(mib, 2, &physmem, &len, NULL, 0) == -1) {
		return -1;
	}

	/* Get UVM statistics for detailed memory info */
	struct uvmexp uvm;
	mib[0] = CTL_VM;
	mib[1] = VM_UVMEXP;
	len = sizeof(uvm);
	if (sysctl(mib, 2, &uvm, &len, NULL, 0) == -1) {
		return -1;
	}

	/* Calculate memory values in bytes */
	unsigned long pagesize = uvm.pagesize;
	unsigned long free_mem = uvm.free * pagesize;
	unsigned long active_mem = uvm.active * pagesize;
	unsigned long inactive_mem = uvm.inactive * pagesize;
	unsigned long wired_mem = uvm.wired * pagesize;

	/* Convert to MB */
	stats->total_mb = physmem / MB;
	stats->free_mb = free_mem / MB;
	stats->active_mb = active_mem / MB;
	stats->inactive_mb = inactive_mem / MB;
	stats->wired_mb = wired_mem / MB;
	stats->cache_mb =
	    0; /* OpenBSD doesn't track cache separately like Linux */

	/* Get swap information */
	struct swapent *swdev;
	int nswap, rnswap;

	nswap = swapctl(SWAP_NSWAP, NULL, 0);
	if (nswap == 0) {
		/* No swap configured */
		stats->swap_total_mb = 0;
		stats->swap_used_mb = 0;
	} else {
		swdev = calloc(nswap, sizeof(*swdev));
		if (swdev == NULL) {
			stats->swap_total_mb = 0;
			stats->swap_used_mb = 0;
		} else {
			rnswap = swapctl(SWAP_STATS, swdev, nswap);
			if (rnswap == -1) {
				stats->swap_total_mb = 0;
				stats->swap_used_mb = 0;
			} else {
				unsigned long swap_total = 0;
				unsigned long swap_used = 0;

				for (int i = 0; i < nswap; i++) {
					if (swdev[i].se_flags & SWF_ENABLE) {
						swap_total += swdev[i].se_nblks;
						swap_used += swdev[i].se_inuse;
					}
				}

				/* Convert from 512-byte blocks to MB */
				stats->swap_total_mb = (swap_total * 512) / MB;
				stats->swap_used_mb = (swap_used * 512) / MB;
			}
			free(swdev);
		}
	}

	return 0;
}

/* Get load average */
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

/* Get OS information */
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

/* Get system uptime using sysctl KERN_BOOTTIME */
int
metrics_get_uptime(char *uptime_str, size_t size)
{
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

	if (days > 0) {
		snprintf(uptime_str, size, "%ld days, %ld:%02ld:%02ld", days,
			 hours, minutes, seconds);
	} else {
		snprintf(uptime_str, size, "%ld:%02ld:%02ld", hours, minutes,
			 seconds);
	}

	return 0;
}

/* Get hostname */
int
metrics_get_hostname(char *hostname, size_t size)
{
	return gethostname(hostname, size);
}

/* Get disk usage information using statfs */
int
metrics_get_disk_usage(DiskInfo *disks, int max_disks)
{
	struct statfs *mntbuf;
	int mntsize, i, count = 0;

	/* Get list of mounted filesystems */
	mntsize = getmntinfo(&mntbuf, MNT_NOWAIT);
	if (mntsize == 0) {
		return 0;
	}

	for (i = 0; i < mntsize && count < max_disks; i++) {
		struct statfs *fs = &mntbuf[i];

		/* Skip special filesystems */
		if (strcmp(fs->f_fstypename, "tmpfs") == 0 ||
		    strcmp(fs->f_fstypename, "procfs") == 0 ||
		    strcmp(fs->f_fstypename, "devfs") == 0 ||
		    strcmp(fs->f_fstypename, "fdescfs") == 0) {
			continue;
		}

		/* Skip if no blocks */
		if (fs->f_blocks == 0) {
			continue;
		}

		DiskInfo *disk = &disks[count];

		/* Copy device name and mount point */
		strlcpy(disk->device, fs->f_mntfromname, sizeof(disk->device));
		strlcpy(disk->mount_point, fs->f_mntonname,
			sizeof(disk->mount_point));

		/* Calculate sizes in MB */
		unsigned long total = fs->f_blocks * fs->f_bsize;
		unsigned long available = fs->f_bavail * fs->f_bsize;
		unsigned long used = total - available;

		disk->total_mb = total / MB;
		disk->used_mb = used / MB;

		/* Calculate percentage */
		if (disk->total_mb > 0) {
			disk->percent_used =
			    (int)((disk->used_mb * 100) / disk->total_mb);
		} else {
			disk->percent_used = 0;
		}

		count++;
	}

	return count;
}

/* Get top listening ports using netstat command */
int
metrics_get_top_ports(PortInfo *ports, int max_ports)
{
	FILE *fp;
	char buffer[512];
	int port_count = 0;

	/* Use netstat to get listening ports - OpenBSD format */
	fp = popen("netstat -an -f inet -p tcp 2>/dev/null | grep LISTEN", "r");
	if (fp == NULL) {
		return 0;
	}

	while (fgets(buffer, sizeof(buffer), fp) != NULL &&
	       port_count < max_ports) {
		char proto[16], recv_q[16], send_q[16];
		char local_addr[128], foreign_addr[128], state[32];

		/* Parse OpenBSD netstat output:
		 * tcp   0   0  127.0.0.1.9001        *.* LISTEN tcp   0   0
		 * *.22                  *.*                    LISTEN
		 */
		if (sscanf(buffer, "%15s %15s %15s %127s %127s %31s", proto,
			   recv_q, send_q, local_addr, foreign_addr,
			   state) == 6) {

			/* Extract port from local address */
			char *port_str = strrchr(local_addr, '.');
			if (port_str != NULL) {
				int port_num = atoi(port_str + 1);

				if (port_num > 0 && port_num <= 65535) {
					/* Check if we already have this port */
					int duplicate = 0;
					for (int i = 0; i < port_count; i++) {
						if (ports[i].port == port_num) {
							duplicate = 1;
							break;
						}
					}

					if (!duplicate) {
						PortInfo *port_info =
						    &ports[port_count];
						port_info->port = port_num;
						strlcpy(
						    port_info->protocol, proto,
						    sizeof(
							port_info->protocol));
						strlcpy(
						    port_info->state, state,
						    sizeof(port_info->state));
						port_info->connection_count =
						    1; /* Simplified */

						port_count++;
					}
				}
			}
		}
	}

	pclose(fp);

	/* Also check UDP if we have space */
	if (port_count < max_ports) {
		fp = popen("netstat -an -f inet -p udp 2>/dev/null | grep -E "
			   "'\\*\\.[0-9]+'",
			   "r");
		if (fp != NULL) {
			while (fgets(buffer, sizeof(buffer), fp) != NULL &&
			       port_count < max_ports) {
				char proto[16], recv_q[16], send_q[16];
				char local_addr[128], foreign_addr[128];

				if (sscanf(buffer, "%15s %15s %15s %127s %127s",
					   proto, recv_q, send_q, local_addr,
					   foreign_addr) >= 4) {

					char *port_str =
					    strrchr(local_addr, '.');
					if (port_str != NULL) {
						int port_num =
						    atoi(port_str + 1);

						if (port_num > 0 &&
						    port_num <= 65535) {
							int duplicate = 0;
							for (int i = 0;
							     i < port_count;
							     i++) {
								if (ports[i]
									.port ==
								    port_num) {
									duplicate =
									    1;
									break;
								}
							}

							if (!duplicate) {
								PortInfo *port_info =
								    &ports
									[port_count];
								port_info
								    ->port =
								    port_num;
								strlcpy(
								    port_info
									->protocol,
								    "udp",
								    sizeof(
									port_info
									    ->protocol));
								strlcpy(
								    port_info
									->state,
								    "active",
								    sizeof(
									port_info
									    ->state));
								port_info
								    ->connection_count =
								    1;

								port_count++;
							}
						}
					}
				}
			}
			pclose(fp);
		}
	}

	return port_count;
}

/* Get network interfaces using getifaddrs */
int
metrics_get_network_interfaces(NetworkInterface *interfaces, int max_interfaces)
{
	struct ifaddrs *ifaddr, *ifa;

	if (getifaddrs(&ifaddr) == -1) {
		return 0;
	}

	int count = 0;

	for (ifa = ifaddr; ifa != NULL && count < max_interfaces;
	     ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL)
			continue;

		/* Only process IPv4 and IPv6 addresses */
		if (ifa->ifa_addr->sa_family != AF_INET &&
		    ifa->ifa_addr->sa_family != AF_INET6) {
			continue;
		}

		/* Check if we already have this interface - if so, skip if it
		 * already has an IP */
		int found_idx = -1;
		for (int i = 0; i < count; i++) {
			if (strcmp(interfaces[i].name, ifa->ifa_name) == 0) {
				found_idx = i;
				break;
			}
		}

		if (found_idx >= 0) {
			/* Interface already exists - if it has no IP yet,
			 * update it */
			if (strcmp(interfaces[found_idx].ip_address, "N/A") ==
			    0) {
				NetworkInterface *intf = &interfaces[found_idx];

				if (ifa->ifa_addr->sa_family == AF_INET) {
					struct sockaddr_in *addr =
					    (struct sockaddr_in *)ifa->ifa_addr;
					inet_ntop(AF_INET, &addr->sin_addr,
						  intf->ip_address,
						  sizeof(intf->ip_address));
				} else if (ifa->ifa_addr->sa_family ==
					   AF_INET6) {
					struct sockaddr_in6 *addr =
					    (struct sockaddr_in6 *)
						ifa->ifa_addr;
					inet_ntop(AF_INET6, &addr->sin6_addr,
						  intf->ip_address,
						  sizeof(intf->ip_address));
				}
			}
			/* Otherwise, interface already has an IP - skip this
			 * entry */
			continue;
		}

		/* New interface - add it */
		NetworkInterface *intf = &interfaces[count];
		strlcpy(intf->name, ifa->ifa_name, sizeof(intf->name));

		/* Get IP address (prefer IPv4) */
		if (ifa->ifa_addr->sa_family == AF_INET) {
			struct sockaddr_in *addr =
			    (struct sockaddr_in *)ifa->ifa_addr;
			inet_ntop(AF_INET, &addr->sin_addr, intf->ip_address,
				  sizeof(intf->ip_address));
		} else if (ifa->ifa_addr->sa_family == AF_INET6) {
			struct sockaddr_in6 *addr =
			    (struct sockaddr_in6 *)ifa->ifa_addr;
			inet_ntop(AF_INET6, &addr->sin6_addr, intf->ip_address,
				  sizeof(intf->ip_address));
		} else {
			strlcpy(intf->ip_address, "N/A",
				sizeof(intf->ip_address));
		}

		/* Get interface status */
		if (ifa->ifa_flags & IFF_UP) {
			if (ifa->ifa_flags & IFF_RUNNING) {
				strlcpy(intf->status, "up",
					sizeof(intf->status));
			} else {
				strlcpy(intf->status, "no carrier",
					sizeof(intf->status));
			}
		} else {
			strlcpy(intf->status, "down", sizeof(intf->status));
		}

		count++;
	}

	freeifaddrs(ifaddr);
	return count;
}

/* Get top CPU-consuming processes */
int
metrics_get_top_cpu_processes(ProcessInfo *processes, int max_processes)
{
	FILE *fp;
	char buffer[512];
	int count = 0;

	/* Use ps to get processes sorted by CPU usage */
	fp = popen("/bin/ps -axo user,pid,%cpu,comm | sort -nrk3 | head -10",
		   "r");
	if (fp == NULL) {
		return 0;
	}

	/* Skip header line */
	if (fgets(buffer, sizeof(buffer), fp) == NULL) {
		pclose(fp);
		return 0;
	}

	while (fgets(buffer, sizeof(buffer), fp) != NULL &&
	       count < max_processes) {
		char user[32], command[256];
		int pid;
		float cpu_pct;

		if (sscanf(buffer, "%31s %d %f %255[^\n]", user, &pid, &cpu_pct,
			   command) == 4) {
			ProcessInfo *proc = &processes[count];

			strlcpy(proc->user, user, sizeof(proc->user));
			proc->pid = pid;
			proc->cpu_percent = cpu_pct;
			proc->memory_percent =
			    0.0f; /* Will be filled if needed */
			proc->memory_mb = 0;

			/* Sanitize command name (remove paths, limit length) */
			char *cmd_base = strrchr(command, '/');
			if (cmd_base) {
				strlcpy(proc->command, cmd_base + 1,
					sizeof(proc->command));
			} else {
				strlcpy(proc->command, command,
					sizeof(proc->command));
			}

			count++;
		}
	}

	pclose(fp);
	return count;
}

/* Get top memory-consuming processes */
int
metrics_get_top_memory_processes(ProcessInfo *processes, int max_processes)
{
	FILE *fp;
	char buffer[512];
	int count = 0;

	/* Use ps to get processes sorted by memory usage (RSS) */
	fp = popen(
	    "/bin/ps -axo user,pid,%mem,rss,comm | sort -nrk3 | head -10", "r");
	if (fp == NULL) {
		return 0;
	}

	/* Skip header line */
	if (fgets(buffer, sizeof(buffer), fp) == NULL) {
		pclose(fp);
		return 0;
	}

	while (fgets(buffer, sizeof(buffer), fp) != NULL &&
	       count < max_processes) {
		char user[32], command[256];
		int pid, rss_kb;
		float mem_pct;

		if (sscanf(buffer, "%31s %d %f %d %255[^\n]", user, &pid,
			   &mem_pct, &rss_kb, command) == 5) {
			ProcessInfo *proc = &processes[count];

			strlcpy(proc->user, user, sizeof(proc->user));
			proc->pid = pid;
			proc->cpu_percent = 0.0f; /* Will be filled if needed */
			proc->memory_percent = mem_pct;
			proc->memory_mb = rss_kb / 1024; /* Convert KB to MB */

			/* Sanitize command name */
			char *cmd_base = strrchr(command, '/');
			if (cmd_base) {
				strlcpy(proc->command, cmd_base + 1,
					sizeof(proc->command));
			} else {
				strlcpy(proc->command, command,
					sizeof(proc->command));
			}

			count++;
		}
	}

	pclose(fp);
	return count;
}

/* Get process count statistics */
int
metrics_get_process_stats(int *total, int *running, int *sleeping, int *zombie)
{
	FILE *fp;
	char buffer[512];

	*total = 0;
	*running = 0;
	*sleeping = 0;
	*zombie = 0;

	/* Use ps to get process states */
	fp = popen("/bin/ps -ax -o state", "r");
	if (fp == NULL) {
		return -1;
	}

	/* Skip header */
	if (fgets(buffer, sizeof(buffer), fp) == NULL) {
		pclose(fp);
		return -1;
	}

	/* Count processes by state */
	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		(*total)++;

		char state = buffer[0];
		switch (state) {
		case 'R': /* Running */
			(*running)++;
			break;
		case 'S': /* Sleeping (interruptible) */
		case 'I': /* Idle */
			(*sleeping)++;
			break;
		case 'Z': /* Zombie */
			(*zombie)++;
			break;
		default:
			break;
		}
	}

	pclose(fp);
	return 0;
}

/* ===== FUNZIONI JSON ===== */

static void
append_cpu_stats_json(char *buffer, size_t size)
{
	CpuStats stats;
	if (metrics_get_cpu_stats(&stats) == 0) {
		snprintf(buffer, size,
			 "\"cpu\": {\"user\": %d, \"nice\": %d, \"system\": "
			 "%d, \"interrupt\": %d, \"idle\": %d}",
			 stats.user, stats.nice, stats.system, stats.interrupt,
			 stats.idle);
	} else {
		snprintf(buffer, size,
			 "\"cpu\": {\"user\": 0, \"nice\": 0, \"system\": 0, "
			 "\"interrupt\": 0, \"idle\": 100}");
	}
}

static void
append_memory_stats_json(char *buffer, size_t size)
{
	MemoryStats stats;
	if (metrics_get_memory_stats(&stats) == 0) {
		snprintf(buffer, size,
			 "\"memory\": {"
			 "\"total_mb\": %ld, "
			 "\"free_mb\": %ld, "
			 "\"active_mb\": %ld, "
			 "\"inactive_mb\": %ld, "
			 "\"wired_mb\": %ld, "
			 "\"cache_mb\": %ld, "
			 "\"swap_total_mb\": %ld, "
			 "\"swap_used_mb\": %ld"
			 "}",
			 stats.total_mb, stats.free_mb, stats.active_mb,
			 stats.inactive_mb, stats.wired_mb, stats.cache_mb,
			 stats.swap_total_mb, stats.swap_used_mb);
	} else {
		snprintf(buffer, size,
			 "\"memory\": {\"total_mb\": 0, \"free_mb\": 0}");
	}
}

static void
append_load_average_json(char *buffer, size_t size)
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

static void
append_os_info_json(char *buffer, size_t size)
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

static void
append_uptime_json(char *buffer, size_t size)
{
	char uptime_str[128];
	if (metrics_get_uptime(uptime_str, sizeof(uptime_str)) == 0) {
		snprintf(buffer, size, "\"uptime\": \"%s\"", uptime_str);
	} else {
		snprintf(buffer, size, "\"uptime\": \"unknown\"");
	}
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
		    "{\"device\": \"%s\", \"mount\": \"%s\", \"total_mb\": "
		    "%ld, \"used_mb\": %ld, \"percent\": %d}",
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

		written =
		    snprintf(ptr, size,
			     "{\"user\": \"%s\", "
			     "\"pid\": %d, "
			     "\"cpu_percent\": %.1f, "
			     "\"command\": \"%s\"}",
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

		written =
		    snprintf(ptr, size,
			     "{\"user\": \"%s\", "
			     "\"pid\": %d, "
			     "\"memory_percent\": %.1f, "
			     "\"memory_mb\": %d, "
			     "\"command\": \"%s\"}",
			     processes[i].user, processes[i].pid,
			     processes[i].memory_percent,
			     processes[i].memory_mb, processes[i].command);
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
			 "\"process_stats\": {"
			 "\"total\": %d, "
			 "\"running\": %d, "
			 "\"sleeping\": %d, "
			 "\"zombie\": %d"
			 "}",
			 total, running, sleeping, zombie);
	} else {
		snprintf(buffer, size,
			 "\"process_stats\": {\"total\": 0, \"running\": 0, "
			 "\"sleeping\": 0, \"zombie\": 0}");
	}
}

/* Main function generate all the JSON */
char *
get_system_metrics_json(void)
{
	static char json[JSON_BUFFER_SIZE];
	char timestamp[64];
	char hostname[256];
	time_t now;

	/* Get timestamp e hostname */
	time(&now);
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
		 localtime(&now));

	if (metrics_get_hostname(hostname, sizeof(hostname)) == -1) {
		strlcpy(hostname, "localhost", sizeof(hostname));
	}

	/* Buffer for sections */
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

	/* Generate sections */
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

	/* build the complete JSON */
	snprintf(json, sizeof(json),
		 "{"
		 "\"timestamp\": \"%s\","
		 "\"hostname\": \"%s\","
		 "%s," /* cpu */
		 "%s," /* memory */
		 "%s," /* load */
		 "%s," /* os */
		 "%s," /* uptime */
		 "%s," /* disks */
		 "%s," /* ports */
		 "%s," /* network */
		 "%s," /* top_cpu_processes */
		 "%s," /* top_memory_processes */
		 "%s"  /* process_stats */
		 "}",
		 timestamp, hostname, cpu_json, memory_json, load_json, os_json,
		 uptime_json, disks_json, ports_json, network_json,
		 top_cpu_json, top_mem_json, proc_stats_json);

	return json;
}

/* Metrics API handler for libmicrohttpd */
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

	response = MHD_create_response_from_buffer(
	    strlen(json_response), json_response, MHD_RESPMEM_PERSISTENT);

	MHD_add_response_header(response, "Content-Type", "application/json");
	MHD_add_response_header(response, "Cache-Control",
				"no-cache, no-store, must-revalidate");
	MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");

	ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	MHD_destroy_response(response);

	return ret;
}
