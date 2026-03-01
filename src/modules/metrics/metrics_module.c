
/* metrics_module.c - System metrics collection (Native kqueue version) */

#include <miniweb/router/router.h>
#include <sys/mount.h>
#include <sys/sched.h> /* CPUSTATES, CP_USER, CP_SYS, CP_IDLE, CP_INTR */
#include <sys/socket.h>
#include <sys/swap.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/utsname.h> /* Required for uname() and struct utsname. */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <miniweb/core/config.h>
#include <miniweb/core/heartbeat.h>
#include <miniweb/core/log.h>
#include <miniweb/http/handler.h>
#include <miniweb/http/utils.h>
#include <miniweb/modules/metrics.h>
#include <miniweb/modules/metrics_internal.h>

#define JSON_BUFFER_SIZE 65536
#define MB (1024 * 1024)
#define RING_CAPACITY (1024 * 1024 / sizeof(MetricSample))
#define METRICS_HISTORY_WINDOW 120

/* Logging macro controlled by global configuration. */
#define LOG(...)                                                               \
	do {                                                                   \
		if (config_verbose)                                            \
			log_debug("[METRICS] " __VA_ARGS__);                   \
	} while (0)

/**
 * @brief Internal data structure.
 */
typedef struct {
	MetricSample *buf;
	size_t head;
	size_t count;
	pthread_mutex_t lock;
} MetricRing;

static MetricRing g_metrics_ring;
static pthread_once_t g_metrics_once = PTHREAD_ONCE_INIT;
static int g_metrics_ring_ready = 0;

static int ring_init(MetricRing *r);
static void ring_push(MetricRing *r, const MetricSample *s);
static size_t ring_last(MetricRing *r, size_t n, MetricSample *out);
static void ring_free(MetricRing *r);
static void metrics_ring_bootstrap(void);
static void metrics_heartbeat_cb(void *ctx);
static void metrics_take_sample(MetricSample *sample);
static int metrics_read_cpu_ticks(uint64_t *total_ticks, uint64_t *idle_ticks);
static char *build_system_metrics_json(MetricSample *history,
				       size_t history_count);
static void metrics_snapshot_update(void);

static pthread_mutex_t g_metrics_snapshot_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_metrics_snapshot_json = NULL;
static time_t g_metrics_snapshot_updated_at = 0;

/**
 * @brief Read raw kernel CPU tick counters.
 * @param total_ticks Receives summed ticks across all CPU states.
 * @param idle_ticks Receives idle-state ticks.
 * @return 0 on success, -1 on failure.
 */
static int
metrics_read_cpu_ticks(uint64_t *total_ticks, uint64_t *idle_ticks)
{
#ifdef __OpenBSD__
	int mib[2] = {CTL_KERN, KERN_CPTIME};
	long cp_time[CPUSTATES];
	size_t len = sizeof(cp_time);
	uint64_t total = 0;

	if (sysctl(mib, 2, cp_time, &len, NULL, 0) == -1)
		return -1;

	for (int i = 0; i < CPUSTATES; i++) {
		if (cp_time[i] < 0)
			return -1;
		total += (uint64_t)cp_time[i];
	}

	if (total == 0 || cp_time[CP_IDLE] < 0)
		return -1;

	*total_ticks = total;
	*idle_ticks = (uint64_t)cp_time[CP_IDLE];
	return 0;
#else
	(void)total_ticks;
	(void)idle_ticks;
	return -1;
#endif
}

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
	MemoryStats mem;
	MetricSample prev;
	uint64_t total_ticks;
	uint64_t idle_ticks;
	time_t now = time(NULL);

	memset(sample, 0, sizeof(*sample));
	sample->ts = (int64_t)now;

	if (metrics_read_cpu_ticks(&total_ticks, &idle_ticks) == 0) {
		sample->cpu_total_ticks = total_ticks;
		sample->cpu_idle_ticks = idle_ticks;

		if (ring_last(&g_metrics_ring, 1, &prev) == 1 &&
		    prev.cpu_total_ticks > 0 &&
		    total_ticks > prev.cpu_total_ticks) {
			uint64_t total_delta =
			    total_ticks - prev.cpu_total_ticks;
			uint64_t idle_delta = 0;

			if (idle_ticks >= prev.cpu_idle_ticks)
				idle_delta = idle_ticks - prev.cpu_idle_ticks;

			sample->cpu =
			    (float)(100.0 * (1.0 - ((double)idle_delta /
						    (double)total_delta)));
			if (sample->cpu < 0.0f)
				sample->cpu = 0.0f;
			if (sample->cpu > 100.0f)
				sample->cpu = 100.0f;
		} else {
			sample->cpu = 0.0f;
		}
	}

	if (metrics_get_memory_stats(&mem) == 0) {
		long used = mem.active_mb + mem.wired_mb;
		sample->mem_used = (used > 0) ? (uint32_t)used : 0;
		sample->mem_total =
		    (mem.total_mb > 0) ? (uint32_t)mem.total_mb : 0;
		sample->swap_used =
		    (mem.swap_used_mb > 0) ? (uint32_t)mem.swap_used_mb : 0;
	}
}

/**
 * @brief Heartbeat callback that samples metrics and refreshes snapshot.
 * @param ctx Unused callback context.
 */
static void
metrics_heartbeat_cb(void *ctx)
{
	MetricSample sample;

	(void)ctx;
	metrics_take_sample(&sample);
	ring_push(&g_metrics_ring, &sample);
	metrics_snapshot_update();
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
	/*
	 * heartbeat_register() returns HB_REGISTER_INSERTED (1) on success,
	 * HB_REGISTER_DUPLICATE (0) if the task name was already registered,
	 * and HB_REGISTER_ERROR (-1) on invalid input or a full table.
	 * Both 0 and 1 are non-error outcomes; only abort on -1.
	 */
	if (heartbeat_register(&(struct hb_task){
		.name = "metrics.sample",
		.period_sec = 1,
		.initial_delay_sec = 0,
		.cb = metrics_heartbeat_cb,
		.ctx = NULL,
	    }) < 0) {
		LOG("Failed to register metrics heartbeat task");
		ring_free(&g_metrics_ring);
		return;
	}
	if (heartbeat_start() != 0) {
		LOG("Failed to start heartbeat scheduler");
		ring_free(&g_metrics_ring);
		return;
	}
	g_metrics_ring_ready = 1;
	/* Take a first snapshot immediately so the very first HTTP request
	 * gets a valid (though empty-history) payload without waiting 1s. */
	metrics_snapshot_update();
}

/**
 * @brief Append historical metrics samples JSON to a destination buffer.
 * @param buffer Destination JSON buffer.
 * @param size Destination buffer size.
 * @param history Sample history array.
 * @param count Number of history entries.
 */

/**
 * @brief Metrics get cpu stats.
 * @param stats Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
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

/**
 * @brief Metrics get memory stats.
 * @param stats Parameter used by this function.
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
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
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
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
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
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
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
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
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
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
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
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
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
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
 * @return Returns 0 on success or a negative value on failure unless documented
 * otherwise.
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
 * @brief Build a complete metrics JSON payload.
 * @param history History ring samples in chronological order.
 * @param history_count Number of samples in history.
 * @return Heap-allocated JSON string on success, NULL on failure.
 */
static char *
build_system_metrics_json(MetricSample *history, size_t history_count)
{
	char *json = malloc(JSON_BUFFER_SIZE);
	if (!json) {
		LOG("Failed to allocate JSON buffer");
		return NULL;
	}

	char timestamp[64];
	char hostname[256];
	time_t now;
	struct tm tm_buf;
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

	time(&now);
	struct tm *tm_ptr = localtime_r(&now, &tm_buf);
	if (tm_ptr) {
		strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
			 tm_ptr);
	} else {
		strlcpy(timestamp, "unknown", sizeof(timestamp));
	}

	if (metrics_get_hostname(hostname, sizeof(hostname)) == -1)
		strlcpy(hostname, "localhost", sizeof(hostname));

	metrics_json_append_cpu_stats(cpu_json, sizeof(cpu_json));
	metrics_json_append_memory_stats(memory_json, sizeof(memory_json));
	metrics_json_append_load_average(load_json, sizeof(load_json));
	metrics_json_append_os_info(os_json, sizeof(os_json));
	metrics_json_append_uptime(uptime_json, sizeof(uptime_json));
	metrics_json_append_disk_info(disks_json, sizeof(disks_json));
	metrics_json_append_top_ports(ports_json, sizeof(ports_json));
	metrics_process_append_json_sections(top_cpu_json,
	    sizeof(top_cpu_json), top_mem_json, sizeof(top_mem_json),
	    proc_stats_json, sizeof(proc_stats_json));
	metrics_json_append_history(history_json, sizeof(history_json), history,
				    history_count);

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
		 uptime_json, disks_json, ports_json, top_cpu_json,
		 top_mem_json, proc_stats_json, history_json);
	return json;
}

/**
 * @brief Refresh the cached metrics JSON snapshot from ring history.
 */
static void
metrics_snapshot_update(void)
{
	MetricSample history[METRICS_HISTORY_WINDOW];
	size_t history_count =
	    ring_last(&g_metrics_ring, METRICS_HISTORY_WINDOW, history);
	char *json = build_system_metrics_json(history, history_count);
	if (!json)
		return;

	pthread_mutex_lock(&g_metrics_snapshot_lock);
	free(g_metrics_snapshot_json);
	g_metrics_snapshot_json = json;
	g_metrics_snapshot_updated_at = time(NULL);
	pthread_mutex_unlock(&g_metrics_snapshot_lock);
}

/**
 * @brief Get a stable metrics JSON snapshot for HTTP responses.
 * @return Newly allocated JSON string, or NULL on allocation failure.
 */
char *
get_system_metrics_json(void)
{
	(void)pthread_once(&g_metrics_once, metrics_ring_bootstrap);
	time_t now = time(NULL);
	int should_refresh = 0;

	pthread_mutex_lock(&g_metrics_snapshot_lock);
	if (g_metrics_snapshot_json == NULL) {
		should_refresh = 1;
	} else if (g_metrics_snapshot_updated_at == 0 ||
		   (now - g_metrics_snapshot_updated_at) > 5) {
		/*
		 * Fallback refresh: if the heartbeat has not updated the
		 * snapshot for more than 5 seconds, regenerate it inline so
		 * stale data is never served indefinitely.
		 */
		should_refresh = 1;
	}
	pthread_mutex_unlock(&g_metrics_snapshot_lock);

	if (should_refresh)
		metrics_snapshot_update();

	pthread_mutex_lock(&g_metrics_snapshot_lock);
	if (g_metrics_snapshot_json) {
		char *copy = strdup(g_metrics_snapshot_json);
		pthread_mutex_unlock(&g_metrics_snapshot_lock);
		if (copy)
			return copy;
	}

	char *fallback =
	    g_metrics_snapshot_json ? strdup(g_metrics_snapshot_json) : NULL;
	pthread_mutex_unlock(&g_metrics_snapshot_lock);
	return fallback;
}

/**
 * @brief Handle GET /api/metrics by returning the latest metrics snapshot.
 * @param req Request context used by the HTTP layer.
 * @return HTTP send status code from the response layer.
 */
int
metrics_handler(http_request_t *req)
{
	char *json = get_system_metrics_json();
	if (!json) {
		return http_send_error(req, 500, "Unable to generate metrics");
	}

	http_response_t *resp = http_response_create();
	if (!resp) {
		free(json);
		return http_send_error(req, 500, "Unable to allocate response");
	}
	resp->status_code = 200;
	resp->content_type = "application/json";

	/* Allow access from external dashboards. */
	http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
	http_response_add_header(resp, "Cache-Control", "no-store");

	/* Attach JSON as response body.
	 *      The '1' flag tells the response layer to free memory
	 * automatically. */
	http_response_set_body(resp, json, strlen(json), 1);

	int ret = http_response_send(req, resp);
	http_response_free(resp);

	return ret;
}

/**
 * @brief Attach metrics API routes to the router.
 * @param r Router to receive route registrations.
 * @return 0 on success, -1 on registration failure.
 */
int
metrics_module_attach_routes(struct router *r)
{
	if (router_register(r, "GET", "/api/metrics", metrics_handler) != 0)
		return -1;

	/* Compatibility alias for clients that call singular form. */
	return router_register(r, "GET", "/api/metric", metrics_handler);
}
