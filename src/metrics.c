/* metrics.c - System metrics collection (Native kqueue version) */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/sched.h>   /* CPUSTATES, CP_USER, CP_SYS, CP_IDLE, CP_INTR */
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/swap.h>
#include <sys/utsname.h>  /* Required for uname() and struct utsname. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <pwd.h>          /* For struct passwd (process usernames). */

#include "../include/metrics.h"
#include "../include/config.h"
#include "../include/http_handler.h"
#include "../include/http_utils.h"

#define JSON_BUFFER_SIZE 65536
#define MB (1024 * 1024)
#define RING_CAPACITY (1024 * 1024 / sizeof(MetricSample))
#define METRICS_HISTORY_WINDOW 120



/* Logging macro controlled by global configuration. */
#define LOG(...)                                            \
do {                                                        \
	if (config_verbose) {                                   \
		fprintf(stderr, "[METRICS] " __VA_ARGS__);          \
		fprintf(stderr, "\n");                              \
	}                                                       \
} while (0)


static struct kinfo_proc *get_procs_snapshot(size_t *nprocs);
static void append_process_json_sections(char *top_cpu_json,
	size_t top_cpu_json_size,
	char *top_mem_json,
	size_t top_mem_json_size,
	char *proc_stats_json,
	size_t proc_stats_json_size);

typedef struct {
	int64_t ts;
	float cpu;
	uint32_t mem_used;
	uint32_t mem_total;
	uint32_t swap_used;
	uint32_t net_rx;
	uint32_t net_tx;
} MetricSample;

typedef struct {
	MetricSample *buf;
	size_t head;
	size_t count;
	pthread_mutex_t lock;
} MetricRing;

static MetricRing g_metrics_ring;
static pthread_t g_metrics_thread;
static pthread_once_t g_metrics_once = PTHREAD_ONCE_INIT;
static int g_metrics_ring_ready = 0;

static int ring_init(MetricRing *r);
static void ring_push(MetricRing *r, const MetricSample *s);
static size_t ring_last(MetricRing *r, size_t n, MetricSample *out);
static void ring_free(MetricRing *r);
static void metrics_ring_bootstrap(void);
static void *metrics_sampler_thread(void *arg);
static void metrics_take_sample(MetricSample *sample);
static void append_metrics_history_json(char *buffer, size_t size,
							MetricSample *history, size_t count);

/**
 * @brief Initialize the in-memory metrics ring buffer.
 * @param r Ring structure to initialize.
 * @return 0 on success, -1 on allocation failure.
 */
static int
ring_init(MetricRing *r)
{
	r->buf = malloc(RING_CAPACITY * sizeof(MetricSample));
	if (!r->buf)
		return -1;
	r->head = 0;
	r->count = 0;
	pthread_mutex_init(&r->lock, NULL);
	return 0;
}

/**
 * @brief Push one metric sample into the ring.
 * @param r Ring buffer instance.
 * @param s Sample to append.
 */
static void
ring_push(MetricRing *r, const MetricSample *s)
{
	pthread_mutex_lock(&r->lock);
	r->buf[r->head] = *s;
	r->head = (r->head + 1) % RING_CAPACITY;
	if (r->count < RING_CAPACITY)
		r->count++;
	pthread_mutex_unlock(&r->lock);
}

/**
 * @brief Copy the last N samples from the ring in chronological order.
 * @param r Ring buffer instance.
 * @param n Requested sample count.
 * @param out Destination array for copied samples.
 * @return Number of samples copied.
 */
static size_t
ring_last(MetricRing *r, size_t n, MetricSample *out)
{
	if (!g_metrics_ring_ready || r->buf == NULL)
		return 0;

	pthread_mutex_lock(&r->lock);

	if (n > r->count)
		n = r->count;

	size_t start = (r->head + RING_CAPACITY - n) % RING_CAPACITY;
	for (size_t i = 0; i < n; i++)
		out[i] = r->buf[(start + i) % RING_CAPACITY];

	pthread_mutex_unlock(&r->lock);
	return n;
}

/**
 * @brief Release resources associated with a metrics ring.
 * @param r Ring buffer instance to reset.
 */
static void
ring_free(MetricRing *r)
{
	free(r->buf);
	pthread_mutex_destroy(&r->lock);
	r->buf = NULL;
	r->count = 0;
	r->head = 0;
}

/**
 * @brief Collect one composite metrics sample.
 * @param sample Destination sample object.
 */
static void
metrics_take_sample(MetricSample *sample)
{
	CpuStats cpu;
	MemoryStats mem;
	time_t now = time(NULL);

	memset(sample, 0, sizeof(*sample));
	sample->ts = (int64_t)now;

	if (metrics_get_cpu_stats(&cpu) == 0)
		sample->cpu = (float)(100 - cpu.idle);

	if (metrics_get_memory_stats(&mem) == 0) {
		long used = mem.active_mb + mem.wired_mb;
		sample->mem_used = (used > 0) ? (uint32_t)used : 0;
		sample->mem_total = (mem.total_mb > 0) ? (uint32_t)mem.total_mb : 0;
		sample->swap_used =
			(mem.swap_used_mb > 0) ? (uint32_t)mem.swap_used_mb : 0;
	}
}

/**
 * @brief Background thread entrypoint that samples metrics every second.
 * @param arg Unused thread argument.
 * @return Always returns NULL.
 */
static void *
metrics_sampler_thread(void *arg)
{
	(void)arg;
	for (;;) {
		MetricSample sample;
		metrics_take_sample(&sample);
		ring_push(&g_metrics_ring, &sample);
		sleep(1);
	}
	return NULL;
}

/**
 * @brief Initialize and start the background metrics sampler.
 */
static void
metrics_ring_bootstrap(void)
{
	if (ring_init(&g_metrics_ring) != 0) {
		LOG("Failed to allocate 1MB metrics ring");
		return;
	}
	if (pthread_create(&g_metrics_thread, NULL,
		metrics_sampler_thread, NULL) != 0) {
		LOG("Failed to start metrics sampler thread");
		ring_free(&g_metrics_ring);
		return;
	}
	g_metrics_ring_ready = 1;
	pthread_detach(g_metrics_thread);
}

/**
 * @brief Append historical metrics samples JSON to a destination buffer.
 * @param buffer Destination JSON buffer.
 * @param size Destination buffer size.
 * @param history Sample history array.
 * @param count Number of history entries.
 */
static void
append_metrics_history_json(char *buffer, size_t size,
	MetricSample *history, size_t count)
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
		written = snprintf(ptr, size,
			"%s{\"ts\": %lld, \"cpu\": %.2f, \"mem_used_mb\": %u, "
			"\"mem_total_mb\": %u, \"swap_used_mb\": %u, "
			"\"net_rx\": %u, \"net_tx\": %u}",
			(i > 0) ? ", " : "",
			(long long)history[i].ts, history[i].cpu,
			history[i].mem_used, history[i].mem_total,
			history[i].swap_used, history[i].net_rx, history[i].net_tx);
		if (written < 0 || (size_t)written >= size)
			break;
		ptr += written;
		size -= (size_t)written;
	}

	if (size > 0)
		snprintf(ptr, size, "]");
}

/* qsort comparator: sort by descending RSS. */
/**
 * @brief Compare memory.
 * @param a Parameter used by this function.
 * @param b Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
static int
compare_memory(const void *a, const void *b)
{
	const struct kinfo_proc *pa = a;
	const struct kinfo_proc *pb = b;

	/* p_vm_rssize is resident set size in pages. */
	if (pa->p_vm_rssize < pb->p_vm_rssize)
		return 1;
	if (pa->p_vm_rssize > pb->p_vm_rssize)
		return -1;
	return 0;
}

/* Helper to snapshot all running processes. */
/**
 * @brief Get procs snapshot.
 * @param nprocs Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
static struct kinfo_proc *
get_procs_snapshot(size_t *nprocs)
{
	int mib[6];
	size_t size;
	struct kinfo_proc *kp = NULL; /* Initialize to NULL. */
	int retry = 0;
	size_t elem_size;
	int nelem;

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_ALL;
	mib[3] = 0;
	mib[4] = sizeof(struct kinfo_proc);
	mib[5] = 0;

	elem_size = sizeof(struct kinfo_proc);

	for (retry = 0; retry < 4; retry++) {
		if (sysctl(mib, 6, NULL, &size, NULL, 0) == -1) {
			LOG("sysctl size query failed: %s", strerror(errno));
			return NULL;
		}

		nelem = size / elem_size;
		nelem = nelem * (5 + retry) / 4;
		size = nelem * elem_size;
		mib[5] = nelem;

		kp = malloc(size);
		if (kp == NULL) {
			LOG("malloc failed for %zu bytes", size);
			return NULL;
		}

		if (sysctl(mib, 6, kp, &size, NULL, 0) == 0) {
			*nprocs = size / elem_size;
			LOG("Successfully retrieved %zu processes (retry %d, "
			"requested %d)",
			*nprocs, retry, nelem);
			return kp;
		}

		if (errno != ENOMEM) {
			LOG("sysctl data query failed: %s", strerror(errno));
			free(kp);
			kp = NULL; /* Critical: reset pointer to NULL after free. */
			return NULL;
		}

		LOG("ENOMEM on retry %d (requested %d elements), trying "
		"again...",
		retry, nelem);
		free(kp);
		kp = NULL; /* Critical: reset pointer to NULL after free. */
	}

	LOG("Failed to get process list after %d retries", retry);
	/* kp is guaranteed to be NULL here. */
	return NULL;
}

/**
 * @brief Metrics get cpu stats.
 * @param stats Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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

	stats->user      = (int)((cp_time[CP_USER] * 100) / total);
	stats->nice      = (int)((cp_time[CP_NICE] * 100) / total);
	stats->system    = (int)((cp_time[CP_SYS]  * 100) / total);
	stats->interrupt = (int)((cp_time[CP_INTR] * 100) / total);
	stats->idle      = (int)((cp_time[CP_IDLE] * 100) / total);
	return 0;
	#else
	stats->user = stats->nice = stats->system = 0;
	stats->interrupt = stats->idle = 0;
	return -1;
	#endif
}

/**
 * @brief Metrics get memory stats.
 * @param stats Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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

/**
 * @brief Metrics get load average.
 * @param load Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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

/**
 * @brief Metrics get os info.
 * @param type Parameter used by this function.
 * @param release Parameter used by this function.
 * @param machine Parameter used by this function.
 * @param size Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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

/**
 * @brief Metrics get uptime.
 * @param uptime_str Parameter used by this function.
 * @param size Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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

/**
 * @brief Metrics get hostname.
 * @param hostname Parameter used by this function.
 * @param size Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
int
metrics_get_hostname(char *hostname, size_t size)
{
	return gethostname(hostname, size);
}

/**
 * @brief Metrics get disk usage.
 * @param disks Parameter used by this function.
 * @param max_disks Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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

/**
 * @brief Metrics get top ports.
 * @param ports Parameter used by this function.
 * @param max_ports Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
int
metrics_get_top_ports(PortInfo *ports, int max_ports)
{
	(void)ports;
	(void)max_ports;
	return 0;
}

/**
 * @brief Metrics get network interfaces.
 * @param interfaces Parameter used by this function.
 * @param max_interfaces Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
int
metrics_get_network_interfaces(NetworkInterface *interfaces, int max_interfaces)
{
	(void)interfaces;
	(void)max_interfaces;
	/* Network interfaces are intentionally excluded from /api/metrics.
	 * Networking data is served by the dedicated networking API. */
	return 0;
}

/**
 * @brief Build top-process and process-stats JSON sections.
 * @param top_cpu_json Output buffer for top CPU processes section.
 * @param top_cpu_json_size Size of top_cpu_json.
 * @param top_mem_json Output buffer for top memory processes section.
 * @param top_mem_json_size Size of top_mem_json.
 * @param proc_stats_json Output buffer for process stats section.
 * @param proc_stats_json_size Size of proc_stats_json.
 */
static void
append_process_json_sections(char *top_cpu_json,
	size_t top_cpu_json_size,
	char *top_mem_json,
	size_t top_mem_json_size,
	char *proc_stats_json,
	size_t proc_stats_json_size)
{
	size_t nprocs = 0;
	struct kinfo_proc *kp = get_procs_snapshot(&nprocs);
	if (!kp) {
		snprintf(top_cpu_json, top_cpu_json_size,
				 "\"top_cpu_processes\": []");
		snprintf(top_mem_json, top_mem_json_size,
				 "\"top_memory_processes\": []");
		snprintf(proc_stats_json, proc_stats_json_size,
				 "\"process_stats\": null");
		return;
	}

	/* Build process_stats from the same sysctl snapshot to avoid
	 * extra process-list queries. */
	int total = (int)nprocs;
	int running = 0, sleeping = 0, zombie = 0;
	for (size_t i = 0; i < nprocs; i++) {
		if (kp[i].p_stat == SRUN || kp[i].p_stat == SONPROC)
			running++;
		else if (kp[i].p_stat == SSLEEP)
			sleeping++;
		else if (kp[i].p_stat == SZOMB)
			zombie++;
	}
	snprintf(proc_stats_json, proc_stats_json_size,
			 "\"process_stats\": {\"total\": %d, \"running\": %d, "
			 "\"sleeping\": %d, \"zombie\": %d}",
		 total, running, sleeping, zombie);

	/* Top CPU. */
	ProcessInfo top_cpu[10];
	int top_cpu_count = 0;
	for (size_t i = 0; i < nprocs && top_cpu_count < 10; i++) {
		if (kp[i].p_stat == SZOMB)
			continue;
		top_cpu[top_cpu_count].pid = kp[i].p_pid;
		top_cpu[top_cpu_count].cpu_percent =
			(100.0f * kp[i].p_pctcpu) / FSCALE;
		strlcpy(top_cpu[top_cpu_count].command, kp[i].p_comm,
			sizeof(top_cpu[top_cpu_count].command));

		struct passwd pwd;
		struct passwd *result = NULL;
		char pwbuf[1024];
		if (getpwuid_r(kp[i].p_uid, &pwd, pwbuf, sizeof(pwbuf),
			&result) == 0 && result != NULL) {
			strlcpy(top_cpu[top_cpu_count].user, pwd.pw_name,
				sizeof(top_cpu[top_cpu_count].user));
		} else {
			snprintf(top_cpu[top_cpu_count].user,
				sizeof(top_cpu[top_cpu_count].user), "%d",
				kp[i].p_uid);
		}
		top_cpu_count++;
	}

	for (int i = 0; i < top_cpu_count - 1; i++) {
		for (int j = 0; j < top_cpu_count - i - 1; j++) {
			if (top_cpu[j].cpu_percent < top_cpu[j + 1].cpu_percent) {
				ProcessInfo tmp = top_cpu[j];
				top_cpu[j] = top_cpu[j + 1];
				top_cpu[j + 1] = tmp;
			}
		}
	}

	char *cpu_ptr = top_cpu_json;
	size_t cpu_left = top_cpu_json_size;
	int w = snprintf(cpu_ptr, cpu_left, "\"top_cpu_processes\": [");
	cpu_ptr += w;
	cpu_left -= (size_t)w;
	for (int i = 0; i < top_cpu_count && cpu_left > 100; i++) {
		if (i > 0) {
			w = snprintf(cpu_ptr, cpu_left, ", ");
			cpu_ptr += w;
			cpu_left -= (size_t)w;
		}
		w = snprintf(cpu_ptr, cpu_left,
			"{\"user\": \"%s\", \"pid\": %d, \"cpu_percent\": %.1f, "
			"\"command\": \"%s\"}",
			top_cpu[i].user, top_cpu[i].pid, top_cpu[i].cpu_percent,
			top_cpu[i].command);
		cpu_ptr += w;
		cpu_left -= (size_t)w;
	}
	snprintf(cpu_ptr, cpu_left, "]");

	/* Top memory requires a sorted copy, still from same snapshot. */
	struct kinfo_proc *kmem = malloc(nprocs * sizeof(struct kinfo_proc));
	if (!kmem) {
		snprintf(top_mem_json, top_mem_json_size,
				 "\"top_memory_processes\": []");
		free(kp);
		return;
	}
	memcpy(kmem, kp, nprocs * sizeof(struct kinfo_proc));
	qsort(kmem, nprocs, sizeof(struct kinfo_proc), compare_memory);

	MemoryStats mem_stats;
	long total_memory_kb = 0;
	if (metrics_get_memory_stats(&mem_stats) == 0)
		total_memory_kb = mem_stats.total_mb * 1024;
	long page_size = sysconf(_SC_PAGESIZE);

	char *mem_ptr = top_mem_json;
	size_t mem_left = top_mem_json_size;
	w = snprintf(mem_ptr, mem_left, "\"top_memory_processes\": [");
	mem_ptr += w;
	mem_left -= (size_t)w;

	int mem_count = 0;
	for (size_t i = 0; i < nprocs && mem_count < 10 && mem_left > 100; i++) {
		if (kmem[i].p_stat == SZOMB)
			continue;
		ProcessInfo proc;
		proc.pid = kmem[i].p_pid;
		proc.memory_mb = (kmem[i].p_vm_rssize * page_size) / (1024 * 1024);
		if (total_memory_kb > 0) {
			long mem_kb = (kmem[i].p_vm_rssize * page_size) / 1024;
			proc.memory_percent = (100.0f * mem_kb) / total_memory_kb;
		} else {
			proc.memory_percent = 0.0f;
		}
		strlcpy(proc.command, kmem[i].p_comm, sizeof(proc.command));
		struct passwd pwd;
		struct passwd *result = NULL;
		char pwbuf[1024];
		if (getpwuid_r(kmem[i].p_uid, &pwd, pwbuf, sizeof(pwbuf),
			&result) == 0 && result != NULL) {
			strlcpy(proc.user, pwd.pw_name, sizeof(proc.user));
		} else {
			snprintf(proc.user, sizeof(proc.user), "%d", kmem[i].p_uid);
		}
		if (mem_count > 0) {
			w = snprintf(mem_ptr, mem_left, ", ");
			mem_ptr += w;
			mem_left -= (size_t)w;
		}
		w = snprintf(mem_ptr, mem_left,
			"{\"user\": \"%s\", \"pid\": %d, \"memory_percent\": %.1f, "
			"\"memory_mb\": %d, \"command\": \"%s\"}",
			proc.user, proc.pid, proc.memory_percent,
			proc.memory_mb, proc.command);
		mem_ptr += w;
		mem_left -= (size_t)w;
		mem_count++;
	}
	snprintf(mem_ptr, mem_left, "]");

	free(kmem);
	free(kp);
}

/**
 * @brief Metrics get top cpu processes.
 * @param processes Parameter used by this function.
 * @param max_processes Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
int
metrics_get_top_cpu_processes(ProcessInfo *processes, int max_processes)
{
	LOG("Getting top CPU processes (max: %d)...", max_processes);
	size_t nprocs = 0;
	struct kinfo_proc *kp = get_procs_snapshot(&nprocs);
	if (!kp) {
		LOG("ERROR: get_procs_snapshot returned NULL");
		return 0;
	}

	LOG("Processing %zu processes for CPU stats...", nprocs);

	ProcessInfo *temp = calloc(nprocs, sizeof(ProcessInfo));
	if (!temp) {
		free(kp);
		return 0;
	}

	int valid_count = 0;
	for (size_t i = 0; i < nprocs; i++) {
		if (kp[i].p_stat == SZOMB)
			continue;

		temp[valid_count].pid = kp[i].p_pid;
		temp[valid_count].cpu_percent =
		(100.0 * kp[i].p_pctcpu) / FSCALE;
		strlcpy(temp[valid_count].command, kp[i].p_comm,
				sizeof(temp[valid_count].command));

		/* Critical: use getpwuid_r() instead of getpwuid(). */
		struct passwd pwd;
		struct passwd *result;
		char pwbuf[1024];

		if (getpwuid_r(kp[i].p_uid, &pwd, pwbuf, sizeof(pwbuf),
			&result) == 0 &&
			result != NULL) {
			strlcpy(temp[valid_count].user, pwd.pw_name,
					sizeof(temp[valid_count].user));
			} else {
				snprintf(temp[valid_count].user,
						 sizeof(temp[valid_count].user), "%d",
						 kp[i].p_uid);
			}

			valid_count++;
	}

	/* Sort by descending CPU usage. */
	for (int i = 0; i < valid_count - 1; i++) {
		for (int j = 0; j < valid_count - i - 1; j++) {
			if (temp[j].cpu_percent < temp[j + 1].cpu_percent) {
				ProcessInfo swap = temp[j];
				temp[j] = temp[j + 1];
				temp[j + 1] = swap;
			}
		}
	}

	int count = (valid_count < max_processes) ? valid_count : max_processes;
	for (int i = 0; i < count; i++) {
		processes[i] = temp[i];
	}

	LOG("Returning %d CPU processes (valid: %d, total: %zu)", count,
		valid_count, nprocs);
	free(temp);
	free(kp);
	return count;
}

/**
 * @brief Metrics get top memory processes.
 * @param processes Parameter used by this function.
 * @param max_processes Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
int
metrics_get_top_memory_processes(ProcessInfo *processes, int max_processes)
{
	LOG("Getting top memory processes (max: %d)...", max_processes);
	size_t nprocs = 0;
	struct kinfo_proc *kp = get_procs_snapshot(&nprocs);
	if (!kp) {
		LOG("ERROR: get_procs_snapshot returned NULL");
		return 0;
	}

	LOG("Processing %zu processes for memory stats...", nprocs);

	qsort(kp, nprocs, sizeof(struct kinfo_proc), compare_memory);

	MemoryStats mem_stats;
	long total_memory_kb = 0;
	if (metrics_get_memory_stats(&mem_stats) == 0) {
		total_memory_kb = mem_stats.total_mb * 1024;
	}

	int count = 0;
	long page_size = sysconf(_SC_PAGESIZE);

	for (size_t i = 0; i < nprocs && count < max_processes; i++) {
		if (kp[i].p_stat == SZOMB)
			continue;

		processes[count].pid = kp[i].p_pid;
		processes[count].memory_mb =
		(kp[i].p_vm_rssize * page_size) / (1024 * 1024);

		if (total_memory_kb > 0) {
			long mem_kb = (kp[i].p_vm_rssize * page_size) / 1024;
			processes[count].memory_percent =
			(100.0 * mem_kb) / total_memory_kb;
		} else {
			processes[count].memory_percent = 0.0;
		}

		strlcpy(processes[count].command, kp[i].p_comm,
				sizeof(processes[count].command));

		/* Critical: use getpwuid_r() instead of getpwuid(). */
		struct passwd pwd;
		struct passwd *result;
		char pwbuf[1024];

		if (getpwuid_r(kp[i].p_uid, &pwd, pwbuf, sizeof(pwbuf),
			&result) == 0 &&
			result != NULL) {
			strlcpy(processes[count].user, pwd.pw_name,
					sizeof(processes[count].user));
			} else {
				snprintf(processes[count].user,
						 sizeof(processes[count].user), "%d",
						 kp[i].p_uid);
			}

			count++;
	}

	LOG("Returning %d memory processes (total: %zu)", count, nprocs);
	free(kp);
	return count;
}

/* 1. Aggregate process counters. */
/**
 * @brief Metrics get process stats.
 * @param total Parameter used by this function.
 * @param running Parameter used by this function.
 * @param sleeping Parameter used by this function.
 * @param zombie Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
int
metrics_get_process_stats(int *total, int *running, int *sleeping, int *zombie)
{
	size_t nprocs = 0;
	struct kinfo_proc *kp = get_procs_snapshot(&nprocs);
	if (!kp)
		return -1;

	*total = (int)nprocs;
	*running = *sleeping = *zombie = 0;
	for (size_t i = 0; i < nprocs; i++) {
		if (kp[i].p_stat == SRUN || kp[i].p_stat == SONPROC)
			(*running)++;
		else if (kp[i].p_stat == SSLEEP)
			(*sleeping)++;
		else if (kp[i].p_stat == SZOMB)
			(*zombie)++;
	}
	free(kp);
	return 0;
}

/**
 * @brief Append cpu stats json.
 * @param buffer Parameter used by this function.
 * @param size Parameter used by this function.
 */
static void
append_cpu_stats_json(char *buffer, size_t size)
{
	CpuStats stats;
	if (metrics_get_cpu_stats(&stats) == 0) {
		int used = stats.user + stats.nice + stats.system + stats.interrupt;
		snprintf(buffer, size,
				 "\"cpu\": {"
				 "\"used_pct\": %d,"
				 "\"user_pct\": %d,"
				 "\"nice_pct\": %d,"
				 "\"system_pct\": %d,"
				 "\"interrupt_pct\": %d,"
				 "\"idle_pct\": %d"
				 "}",
		   used,
		   stats.user, stats.nice,
		   stats.system, stats.interrupt,
		   stats.idle);
	} else {
		snprintf(buffer, size, "\"cpu\": null");
	}
}

/**
 * @brief Append memory stats json.
 * @param buffer Parameter used by this function.
 * @param size Parameter used by this function.
 */
static void
append_memory_stats_json(char *buffer, size_t size)
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
 * @brief Append load average json.
 * @param buffer Parameter used by this function.
 * @param size Parameter used by this function.
 */
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

/**
 * @brief Append os info json.
 * @param buffer Parameter used by this function.
 * @param size Parameter used by this function.
 */
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

/**
 * @brief Append uptime json.
 * @param buffer Parameter used by this function.
 * @param size Parameter used by this function.
 */
static void
append_uptime_json(char *buffer, size_t size)
{
	char uptime_str[128];
	if (metrics_get_uptime(uptime_str, sizeof(uptime_str)) == 0)
		snprintf(buffer, size, "\"uptime\": \"%s\"", uptime_str);
	else
		snprintf(buffer, size, "\"uptime\": \"unknown\"");
}

/**
 * @brief Append disk info json.
 * @param buffer Parameter used by this function.
 * @param size Parameter used by this function.
 */
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

/**
 * @brief Append top ports json.
 * @param buffer Parameter used by this function.
 * @param size Parameter used by this function.
 */
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

/**
 * @brief Append network interfaces json.
 * @param buffer Parameter used by this function.
 * @param size Parameter used by this function.
 */
/**
 * @brief Get system metrics json.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
char *
get_system_metrics_json(void)
{
	(void)pthread_once(&g_metrics_once, metrics_ring_bootstrap);

	char *json = malloc(JSON_BUFFER_SIZE);
	if (!json) {
		LOG("Failed to allocate JSON buffer");
		return NULL;
	}

	char timestamp[64];
	char hostname[256];
	time_t now;
	struct tm tm_buf; /* Buffer for localtime_r(). */

	char cpu_json[256];
	char memory_json[512];
	char load_json[256];
	char os_json[512];
	char uptime_json[256];
	char disks_json[2048];
	char ports_json[2048];
	char top_cpu_json[2048];
	char top_mem_json[2048];
	char proc_stats_json[256];
	char history_json[32768];
	MetricSample history[METRICS_HISTORY_WINDOW];
	size_t history_count = 0;

	time(&now);

	/* Critical: use localtime_r() instead of localtime(). */
	struct tm *tm_ptr = localtime_r(&now, &tm_buf);
	if (tm_ptr) {
		strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
				 tm_ptr);
	} else {
		strlcpy(timestamp, "unknown", sizeof(timestamp));
	}

	if (metrics_get_hostname(hostname, sizeof(hostname)) == -1)
		strlcpy(hostname, "localhost", sizeof(hostname));

	append_cpu_stats_json(cpu_json, sizeof(cpu_json));
	append_memory_stats_json(memory_json, sizeof(memory_json));
	append_load_average_json(load_json, sizeof(load_json));
	append_os_info_json(os_json, sizeof(os_json));
	append_uptime_json(uptime_json, sizeof(uptime_json));
	append_disk_info_json(disks_json, sizeof(disks_json));
	append_top_ports_json(ports_json, sizeof(ports_json));
	append_process_json_sections(top_cpu_json, sizeof(top_cpu_json),
		top_mem_json, sizeof(top_mem_json),
		proc_stats_json, sizeof(proc_stats_json));
	history_count = ring_last(&g_metrics_ring, METRICS_HISTORY_WINDOW,
		history);
	append_metrics_history_json(history_json, sizeof(history_json),
		history, history_count);

	snprintf(json, JSON_BUFFER_SIZE,
			 "{"
			 "\"timestamp\": \"%s\","
			 "\"hostname\": \"%s\","
			 "%s,"
			 "%s,"
			 "%s,"
			 "%s,"
			 "%s,"
			 "%s,"
			 "%s,"
		 "%s,"
		 "%s,"
		 "%s,"
		 "%s"
		 "}",
	  timestamp, hostname, cpu_json, memory_json, load_json, os_json,
	  uptime_json, disks_json, ports_json,
	  top_cpu_json, top_mem_json, proc_stats_json, history_json);

	return json;
}


/**
 * Handler HTTP
 */
int
metrics_handler(http_request_t *req)
{
	char *json = get_system_metrics_json();
	if (!json) {
		return http_send_error(req, 500, "Unable to generate metrics");
	}

	http_response_t *resp = http_response_create();
	resp->status_code = 200;
	resp->content_type = "application/json";

	/* Allow access from external dashboards. */
	http_response_add_header(resp, "Access-Control-Allow-Origin", "*");

	/* Attach JSON as response body.
	 *      The '1' flag tells the response layer to free memory automatically. */
	http_response_set_body(resp, json, strlen(json), 1);

	int ret = http_response_send(req, resp);
	http_response_free(resp);

	return ret;
}
