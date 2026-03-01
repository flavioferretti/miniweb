/* metrics_process.c - Process snapshot and process JSON helpers */

#include <miniweb/core/config.h>
#include <miniweb/core/log.h>
#include <miniweb/modules/metrics.h>
#include <miniweb/modules/metrics_internal.h>

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/sysctl.h>

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG(...)                                                               \
	do {                                                                   \
		if (config_verbose)                                            \
			log_debug("[METRICS] " __VA_ARGS__);                   \
	} while (0)

/**
 * @brief Compare process entries by resident memory size in descending order.
 * @param a Left-hand process entry.
 * @param b Right-hand process entry.
 * @return Negative when @p a should come first, positive when @p b should
 * come first, or 0 when equal.
 */
static int
compare_memory_desc(const void *a, const void *b)
{
	const struct kinfo_proc *pa = a;
	const struct kinfo_proc *pb = b;

	if (pa->p_vm_rssize < pb->p_vm_rssize)
		return 1;
	if (pa->p_vm_rssize > pb->p_vm_rssize)
		return -1;
	return 0;
}

/**
 * @brief Resolve a user name for a process uid.
 * @param uid Process user identifier.
 * @param user Destination output buffer.
 * @param size Destination buffer size.
 */
static void
metrics_resolve_username(uid_t uid, char *user, size_t size)
{
	struct passwd pwd;
	struct passwd *result = NULL;
	char pwbuf[1024];

	if (getpwuid_r(uid, &pwd, pwbuf, sizeof(pwbuf), &result) == 0 &&
	    result != NULL) {
		strlcpy(user, pwd.pw_name, size);
		return;
	}
	snprintf(user, size, "%u", (unsigned int)uid);
}

/**
 * @brief Snapshot the current process table.
 * @param nprocs Receives number of process entries.
 * @return Heap-allocated process array on success, NULL on failure.
 */
static struct kinfo_proc *
metrics_get_procs_snapshot(size_t *nprocs)
{
	int mib[6];
	size_t size;
	struct kinfo_proc *kp = NULL;
	int retry;
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
			return kp;
		}

		if (errno != ENOMEM) {
			LOG("sysctl data query failed: %s", strerror(errno));
			free(kp);
			return NULL;
		}
		free(kp);
		kp = NULL;
	}

	LOG("Failed to get process list after %d retries", retry);
	return NULL;
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
void
metrics_process_append_json_sections(char *top_cpu_json, size_t top_cpu_json_size,
    char *top_mem_json, size_t top_mem_json_size, char *proc_stats_json,
    size_t proc_stats_json_size)
{
	size_t nprocs = 0;
	struct kinfo_proc *kp = metrics_get_procs_snapshot(&nprocs);
	if (!kp) {
		snprintf(top_cpu_json, top_cpu_json_size,
		    "\"top_cpu_processes\": []");
		snprintf(top_mem_json, top_mem_json_size,
		    "\"top_memory_processes\": []");
		snprintf(proc_stats_json, proc_stats_json_size,
		    "\"process_stats\": null");
		return;
	}

	int total = (int)nprocs;
	int running = 0;
	int sleeping = 0;
	int zombie = 0;
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
	    "\"sleeping\": %d, \"zombie\": %d}", total, running, sleeping,
	    zombie);

	ProcessInfo top_cpu[10];
	int top_cpu_count = 0;
	for (size_t i = 0; i < nprocs && top_cpu_count < 10; i++) {
		if (kp[i].p_stat == SZOMB)
			continue;
		top_cpu[top_cpu_count].pid = kp[i].p_pid;
		top_cpu[top_cpu_count].cpu_percent = (100.0f * kp[i].p_pctcpu) / FSCALE;
		strlcpy(top_cpu[top_cpu_count].command, kp[i].p_comm,
		    sizeof(top_cpu[top_cpu_count].command));
		metrics_resolve_username(kp[i].p_uid, top_cpu[top_cpu_count].user,
		    sizeof(top_cpu[top_cpu_count].user));
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

	struct kinfo_proc *kmem = malloc(nprocs * sizeof(struct kinfo_proc));
	if (!kmem) {
		snprintf(top_mem_json, top_mem_json_size,
		    "\"top_memory_processes\": []");
		free(kp);
		return;
	}
	memcpy(kmem, kp, nprocs * sizeof(struct kinfo_proc));
	qsort(kmem, nprocs, sizeof(struct kinfo_proc), compare_memory_desc);

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
		metrics_resolve_username(kmem[i].p_uid, proc.user,
		    sizeof(proc.user));
		if (mem_count > 0) {
			w = snprintf(mem_ptr, mem_left, ", ");
			mem_ptr += w;
			mem_left -= (size_t)w;
		}
		w = snprintf(mem_ptr, mem_left,
		    "{\"user\": \"%s\", \"pid\": %d, \"memory_percent\": %.1f, "
		    "\"memory_mb\": %d, \"command\": \"%s\"}",
		    proc.user, proc.pid, proc.memory_percent, proc.memory_mb,
		    proc.command);
		mem_ptr += w;
		mem_left -= (size_t)w;
		mem_count++;
	}
	snprintf(mem_ptr, mem_left, "]");

	free(kmem);
	free(kp);
}

/**
 * @brief Collect top processes sorted by CPU usage.
 * @param processes Output array receiving process rows.
 * @param max_processes Maximum number of processes to return.
 * @return Number of entries written.
 */
int
metrics_get_top_cpu_processes(ProcessInfo *processes, int max_processes)
{
	size_t nprocs = 0;
	struct kinfo_proc *kp = metrics_get_procs_snapshot(&nprocs);
	if (!kp)
		return 0;

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
		temp[valid_count].cpu_percent = (100.0f * kp[i].p_pctcpu) / FSCALE;
		strlcpy(temp[valid_count].command, kp[i].p_comm,
		    sizeof(temp[valid_count].command));
		metrics_resolve_username(kp[i].p_uid, temp[valid_count].user,
		    sizeof(temp[valid_count].user));
		valid_count++;
	}

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
	for (int i = 0; i < count; i++)
		processes[i] = temp[i];

	free(temp);
	free(kp);
	return count;
}

/**
 * @brief Collect top processes sorted by memory usage.
 * @param processes Output array receiving process rows.
 * @param max_processes Maximum number of processes to return.
 * @return Number of entries written.
 */
int
metrics_get_top_memory_processes(ProcessInfo *processes, int max_processes)
{
	size_t nprocs = 0;
	struct kinfo_proc *kp = metrics_get_procs_snapshot(&nprocs);
	if (!kp)
		return 0;

	qsort(kp, nprocs, sizeof(struct kinfo_proc), compare_memory_desc);

	MemoryStats mem_stats;
	long total_memory_kb = 0;
	if (metrics_get_memory_stats(&mem_stats) == 0)
		total_memory_kb = mem_stats.total_mb * 1024;

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
			    (100.0f * mem_kb) / total_memory_kb;
		} else {
			processes[count].memory_percent = 0.0f;
		}
		strlcpy(processes[count].command, kp[i].p_comm,
		    sizeof(processes[count].command));
		metrics_resolve_username(kp[i].p_uid, processes[count].user,
		    sizeof(processes[count].user));
		count++;
	}

	free(kp);
	return count;
}

/**
 * @brief Collect aggregate process counters by scheduler state.
 * @param total Receives total process count.
 * @param running Receives running process count.
 * @param sleeping Receives sleeping process count.
 * @param zombie Receives zombie process count.
 * @return 0 on success, -1 on failure.
 */
int
metrics_get_process_stats(int *total, int *running, int *sleeping, int *zombie)
{
	size_t nprocs = 0;
	struct kinfo_proc *kp = metrics_get_procs_snapshot(&nprocs);
	if (!kp)
		return -1;

	*total = (int)nprocs;
	*running = 0;
	*sleeping = 0;
	*zombie = 0;
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
