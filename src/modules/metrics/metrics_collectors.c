/* metrics_collectors.c - Metrics system collection helpers */

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>

#include <sys/mount.h>
#ifdef __OpenBSD__
#include <sys/sched.h> /* CPUSTATES, CP_USER, CP_SYS, CP_IDLE, CP_INTR */
#include <sys/swap.h>
#include <sys/sysctl.h>
#endif

#include <sys/utsname.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <miniweb/modules/metrics.h>

#define MB (1024 * 1024)

int
metrics_get_cpu_stats(CpuStats *stats)
{
#ifdef __OpenBSD__
	int mib[2] = {CTL_KERN, KERN_CPTIME};
	long cp_time[CPUSTATES];
	size_t len = sizeof(cp_time);

	if (sysctl(mib, 2, cp_time, &len, NULL, 0) == -1) {
		stats->user = stats->nice = stats->system = 0;
		stats->interrupt = stats->idle = 0;
		return -1;
	}

	long total = 0;
	for (int i = 0; i < CPUSTATES; i++)
		total += cp_time[i];
	if (total == 0) {
		stats->user = stats->nice = stats->system = 0;
		stats->interrupt = stats->idle = 0;
		return -1;
	}

	stats->user = (int)((cp_time[CP_USER] * 100) / total);
	stats->nice = (int)((cp_time[CP_NICE] * 100) / total);
	stats->system = (int)((cp_time[CP_SYS] * 100) / total);
	stats->interrupt = (int)((cp_time[CP_INTR] * 100) / total);
	stats->idle = (int)((cp_time[CP_IDLE] * 100) / total);
	return 0;
#else
	stats->user = stats->nice = stats->system = 0;
	stats->interrupt = stats->idle = 0;
	return -1;
#endif
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

	if (getloadavg(loadavg, 3) == -1)
		return -1;

	load->load_1min = loadavg[0];
	load->load_5min = loadavg[1];
	load->load_15min = loadavg[2];
	return 0;
}

int
metrics_get_os_info(char *type, char *release, char *machine, size_t size)
{
	struct utsname uts;

	if (uname(&uts) == -1)
		return -1;

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
	if (days > 0) {
		snprintf(uptime_str, size, "%ld days, %ld:%02ld:%02ld", days,
		    hours, minutes, seconds);
	} else {
		snprintf(uptime_str, size, "%ld:%02ld:%02ld", hours, minutes,
		    seconds);
	}
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
		    strcmp(fs->f_fstypename, "fdescfs") == 0 ||
		    fs->f_blocks == 0)
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
		    disk->total_mb > 0
			? (int)((disk->used_mb * 100) / disk->total_mb)
			: 0;
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
	(void)interfaces;
	(void)max_interfaces;
	/* Network interfaces are intentionally excluded from /api/metrics.
	 * Networking data is served by the dedicated networking API. */
	return 0;
}
