/*
 * networking.c - Network information collection for OpenBSD
 *
 * Collects routing tables, DNS config, and interface statistics
 * WITHOUT requiring root privileges (no pfctl)
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <net/route.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

/* Include del progetto */
#include "../include/networking.h"
#include "../include/template_engine.h"
#include "../include/config.h"
#include "../include/http_handler.h"
#include "../include/log.h"

#ifndef SA_SIZE
#define SA_SIZE(sa)                                                   \
(((sa)->sa_len == 0) ? sizeof(struct sockaddr) : (size_t)(sa)->sa_len)
#endif

/* Verbose logging */
extern int config_verbose;
#define LOG(...)                            \
 do {                                      \
	if (config_verbose)                     \
		log_debug("[NETWORK] " __VA_ARGS__); \
 } while (0)

#define NETWORK_JSON_BUFFER_SIZE 65536
#define NETWORK_RING_BYTES (1024 * 1024)

typedef struct {
	time_t ts;
	RouteEntry routes[50];
	int route_count;
	DnsConfig dns;
	NetStats interfaces[10];
	int interface_count;
} NetworkingSample;

#define NETWORK_RING_CAPACITY ((size_t)(NETWORK_RING_BYTES / sizeof(NetworkingSample)))

typedef struct {
	NetworkingSample *buf;
	size_t head;
	size_t count;
	pthread_mutex_t lock;
} NetworkingRing;

static NetworkingRing g_networking_ring;
static pthread_t g_networking_thread;
static pthread_once_t g_networking_once = PTHREAD_ONCE_INIT;
static int g_networking_ring_ready = 0;

static int networking_ring_init(NetworkingRing *r);
static void networking_ring_push(NetworkingRing *r,
		const NetworkingSample *s);
static int networking_ring_last(NetworkingRing *r, NetworkingSample *out);
static size_t networking_ring_last_n(NetworkingRing *r, size_t n,
	NetworkingSample *out);
static void networking_collect_sample(NetworkingSample *sample);
static void *networking_sampler_thread(void *arg);
static void networking_ring_bootstrap(void);


/* ========================================================================
 * ROUTING TABLE
 * ======================================================================== */

/**
 * @brief Collect kernel routing table entries.
 * @param routes Output array that receives route entries.
 * @param max_routes Maximum number of entries that can be written.
 * @return Number of routes stored, or -1 on fatal errors.
 */
int
networking_get_routes(RouteEntry *routes, int max_routes)
{
	int mib[6];
	size_t needed;
	char *buf, *next, *lim;
	struct rt_msghdr *rtm;
	struct sockaddr *sa;
	struct sockaddr_in *sin;
	struct sockaddr_in6 *sin6;
	// struct sockaddr_dl *sdl;
	struct sockaddr_dl *sdl __attribute__((unused));
	int count = 0;

	LOG("Getting routing table...");

	mib[0] = CTL_NET;
	mib[1] = PF_ROUTE;
	mib[2] = 0;
	mib[3] = 0;
	mib[4] = NET_RT_DUMP;
	mib[5] = 0;

	if (sysctl(mib, 6, NULL, &needed, NULL, 0) < 0) {
		LOG("sysctl routing table size failed: %s", strerror(errno));
		return 0;
	}

	buf = malloc(needed);
	if (buf == NULL) {
		LOG("malloc failed for routing table");
		return 0;
	}

	if (sysctl(mib, 6, buf, &needed, NULL, 0) < 0) {
		LOG("sysctl routing table failed: %s", strerror(errno));
		free(buf);
		return 0;
	}

	lim = buf + needed;
	for (next = buf; next < lim && count < max_routes;
		 next += rtm->rtm_msglen) {
		rtm = (struct rt_msghdr *)next;

	if (rtm->rtm_version != RTM_VERSION) {
		continue;
	}
	if (rtm->rtm_type != RTM_GET && rtm->rtm_type != RTM_ADD) {
		continue;
	}

	sa = (struct sockaddr *)(rtm + 1);

	/* Destination */
	if (rtm->rtm_addrs & RTA_DST) {
		if (sa->sa_family == AF_INET) {
			sin = (struct sockaddr_in *)sa;
			inet_ntop(AF_INET, &sin->sin_addr,
					  routes[count].destination,
			 sizeof(routes[count].destination));
		} else if (sa->sa_family == AF_INET6) {
			sin6 = (struct sockaddr_in6 *)sa;
			inet_ntop(AF_INET6, &sin6->sin6_addr,
					  routes[count].destination,
			 sizeof(routes[count].destination));
		} else {
			strlcpy(routes[count].destination, "-",
					sizeof(routes[count].destination));
		}
		sa = (struct sockaddr *)((char *)sa + SA_SIZE(sa));
	}

	/* Gateway */
	if (rtm->rtm_addrs & RTA_GATEWAY) {
		if (sa->sa_family == AF_INET) {
			sin = (struct sockaddr_in *)sa;
			inet_ntop(AF_INET, &sin->sin_addr,
					  routes[count].gateway,
			 sizeof(routes[count].gateway));
		} else if (sa->sa_family == AF_INET6) {
			sin6 = (struct sockaddr_in6 *)sa;
			inet_ntop(AF_INET6, &sin6->sin6_addr,
					  routes[count].gateway,
			 sizeof(routes[count].gateway));
		} else if (sa->sa_family == AF_LINK) {
			sdl = (struct sockaddr_dl *)sa;
			strlcpy(routes[count].gateway, "link#",
					sizeof(routes[count].gateway));
		} else {
			strlcpy(routes[count].gateway, "-",
					sizeof(routes[count].gateway));
		}
		sa = (struct sockaddr *)((char *)sa + SA_SIZE(sa));
	}

	/* Netmask */
	if (rtm->rtm_addrs & RTA_NETMASK) {
		if (sa->sa_family == AF_INET) {
			sin = (struct sockaddr_in *)sa;
			inet_ntop(AF_INET, &sin->sin_addr,
					  routes[count].netmask,
			 sizeof(routes[count].netmask));
		} else {
			strlcpy(routes[count].netmask, "-",
					sizeof(routes[count].netmask));
		}
		sa = (struct sockaddr *)((char *)sa + SA_SIZE(sa));
	}

	/* Interface */
	if (rtm->rtm_index > 0) {
		if_indextoname(rtm->rtm_index, routes[count].interface);
	}

	/* Flags */
	routes[count].flags = rtm->rtm_flags;
	snprintf(routes[count].flags_str,
			 sizeof(routes[count].flags_str), "%s%s%s%s%s",
			 (rtm->rtm_flags & RTF_UP) ? "U" : "",
			 (rtm->rtm_flags & RTF_GATEWAY) ? "G" : "",
			 (rtm->rtm_flags & RTF_HOST) ? "H" : "",
			 (rtm->rtm_flags & RTF_STATIC) ? "S" : "",
			 (rtm->rtm_flags & RTF_DYNAMIC) ? "D" : "");

	count++;
		 }

		 free(buf);
		 LOG("Retrieved %d routes", count);
		 return count;
}

/* ========================================================================
 * DNS CONFIGURATION
 * ======================================================================== */

/**
 * @brief Parse DNS resolver settings from /etc/resolv.conf.
 * @param config Output structure populated with resolver configuration.
 * @return Returns 0 on success or -1 on failure.
 */
int
networking_get_dns_config(DnsConfig *config)
{
	FILE *fp;
	char line[512];

	LOG("Reading DNS configuration from /etc/resolv.conf...");

	memset(config, 0, sizeof(DnsConfig));

	fp = fopen("/etc/resolv.conf", "r");
	if (!fp) {
		LOG("Failed to open /etc/resolv.conf: %s", strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), fp) && config->nameserver_count < 8) {
		char *p = line;

		/* Skip whitespace */
		while (*p == ' ' || *p == '\t')
			p++;

		/* Skip comments and empty lines */
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;

		if (strncmp(p, "nameserver", 10) == 0) {
			p += 10;
			while (*p == ' ' || *p == '\t')
				p++;

			char *end = strchr(p, '\n');
			if (end)
				*end = '\0';

			strlcpy(config->nameservers[config->nameserver_count], p, sizeof(config->nameservers[0]));
			config->nameserver_count++;

		} else if (strncmp(p, "domain", 6) == 0) {
			p += 6;
			while (*p == ' ' || *p == '\t')
				p++;

			char *end = strchr(p, '\n');
			if (end)
				*end = '\0';

			strlcpy(config->domain, p, sizeof(config->domain));
		} else if (strncmp(p, "search", 6) == 0) {
			p += 6;
			while (*p == ' ' || *p == '\t')
				p++;

			char *end = strchr(p, '\n');
			if (end)
				*end = '\0';

			strlcpy(config->search, p, sizeof(config->search));
		}
	}

	fclose(fp);
	LOG("Found %d nameservers", config->nameserver_count);
	return 0;
}

/* ========================================================================
 * INTERFACE STATISTICS
 * ======================================================================== */

/**
 * @brief Collect per-interface IPv4/IPv6 and traffic statistics.
 * @param stats Output array for interface statistics entries.
 * @param max_interfaces Maximum number of entries to write.
 * @return Number of interfaces captured.
 */
int
networking_get_if_stats(NetStats *stats, int max_interfaces)
{
	struct ifaddrs *ifap, *ifa;
	struct sockaddr_in *sin;
	int count = 0;

	LOG("Getting interface statistics...");

	if (getifaddrs(&ifap) != 0) {
		LOG("getifaddrs failed: %s", strerror(errno));
		return 0;
	}

	for (ifa = ifap; ifa && count < max_interfaces; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL)
			continue;

		if (ifa->ifa_addr->sa_family == AF_LINK) {
			struct if_data *ifd = (struct if_data *)ifa->ifa_data;
			if (ifd == NULL)
				continue;

			memset(&stats[count], 0, sizeof(NetStats));
			strlcpy(stats[count].interface, ifa->ifa_name,
					sizeof(stats[count].interface));
			strlcpy(stats[count].ipv4, "-", sizeof(stats[count].ipv4));

			stats[count].rx_packets = ifd->ifi_ipackets;
			stats[count].rx_bytes = ifd->ifi_ibytes;
			stats[count].rx_errors = ifd->ifi_ierrors;
			stats[count].rx_dropped = ifd->ifi_iqdrops;

			stats[count].tx_packets = ifd->ifi_opackets;
			stats[count].tx_bytes = ifd->ifi_obytes;
			stats[count].tx_errors = ifd->ifi_oerrors;
			stats[count].tx_dropped =
			0; /* Not tracked by OpenBSD */

			count++;
		}
	}

	for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET)
			continue;

		sin = (struct sockaddr_in *)ifa->ifa_addr;
		for (int i = 0; i < count; i++) {
			if (strcmp(stats[i].interface, ifa->ifa_name) != 0)
				continue;
			if (strcmp(stats[i].ipv4, "-") != 0)
				break;
			if (!inet_ntop(AF_INET, &sin->sin_addr, stats[i].ipv4,
					       sizeof(stats[i].ipv4))) {
				strlcpy(stats[i].ipv4, "-", sizeof(stats[i].ipv4));
			}
			break;
		}
	}

	freeifaddrs(ifap);
	LOG("Retrieved stats for %d interfaces", count);
	return count;
}

/* ========================================================================
 * NETWORK CONNECTIONS (netstat equivalent via sysctl)
 * ======================================================================== */

/**
 * @brief Return a snapshot of network connections.
 * @param conns Output array for connection records.
 * @param max_conns Maximum number of records to populate.
 * @return Number of records written, or -1 on error.
 */
int
networking_get_connections(NetworkConnection *conns, int max_conns)
{
	/* This would use sysctl to get TCP/UDP connection table
	 * Similar to: sysctl net.inet.tcp.pcblist
	 * For simplicity, leaving this as a placeholder */
	(void)conns;
	(void)max_conns;

	LOG("Getting network connections (placeholder)...");
	return 0;
}

static int
networking_ring_init(NetworkingRing *r)
{
	r->buf = malloc(NETWORK_RING_CAPACITY * sizeof(NetworkingSample));
	if (!r->buf)
		return -1;
	r->head = 0;
	r->count = 0;
	pthread_mutex_init(&r->lock, NULL);
	return 0;
}

static void
networking_ring_push(NetworkingRing *r, const NetworkingSample *s)
{
	pthread_mutex_lock(&r->lock);
	r->buf[r->head] = *s;
	r->head = (r->head + 1) % NETWORK_RING_CAPACITY;
	if (r->count < NETWORK_RING_CAPACITY)
		r->count++;
	pthread_mutex_unlock(&r->lock);
}

static int
networking_ring_last(NetworkingRing *r, NetworkingSample *out)
{
	if (!g_networking_ring_ready || r->buf == NULL)
		return 0;

	pthread_mutex_lock(&r->lock);
	if (r->count == 0) {
		pthread_mutex_unlock(&r->lock);
		return 0;
	}

	size_t idx = (r->head + NETWORK_RING_CAPACITY - 1) % NETWORK_RING_CAPACITY;
	*out = r->buf[idx];
	pthread_mutex_unlock(&r->lock);
	return 1;
}

static size_t
networking_ring_last_n(NetworkingRing *r, size_t n, NetworkingSample *out)
{
	if (!g_networking_ring_ready || r->buf == NULL || out == NULL || n == 0)
		return 0;

	pthread_mutex_lock(&r->lock);
	size_t available = r->count;
	size_t to_copy = n < available ? n : available;
	if (to_copy == 0) {
		pthread_mutex_unlock(&r->lock);
		return 0;
	}

	size_t start = (r->head + NETWORK_RING_CAPACITY - to_copy) % NETWORK_RING_CAPACITY;
	for (size_t i = 0; i < to_copy; i++) {
		size_t idx = (start + i) % NETWORK_RING_CAPACITY;
		out[i] = r->buf[idx];
	}
	pthread_mutex_unlock(&r->lock);
	return to_copy;
}

static void
networking_collect_sample(NetworkingSample *sample)
{
	memset(sample, 0, sizeof(*sample));
	time(&sample->ts);
	sample->route_count = networking_get_routes(sample->routes, 50);
	if (sample->route_count < 0)
		sample->route_count = 0;
	if (networking_get_dns_config(&sample->dns) != 0)
		memset(&sample->dns, 0, sizeof(sample->dns));
	sample->interface_count = networking_get_if_stats(sample->interfaces, 10);
	if (sample->interface_count < 0)
		sample->interface_count = 0;
}

static void *
networking_sampler_thread(void *arg)
{
	(void)arg;
	for (;;) {
		NetworkingSample sample;

		networking_collect_sample(&sample);
		networking_ring_push(&g_networking_ring, &sample);
		sleep(1);
	}

	return NULL;
}

static void
networking_ring_bootstrap(void)
{
	if (networking_ring_init(&g_networking_ring) != 0) {
		LOG("Failed to allocate 1MB networking ring");
		return;
	}

	if (pthread_create(&g_networking_thread, NULL,
		networking_sampler_thread, NULL) != 0) {
		LOG("Failed to start networking sampler thread");
		free(g_networking_ring.buf);
		g_networking_ring.buf = NULL;
		return;
	}

	g_networking_ring_ready = 1;
	pthread_detach(g_networking_thread);
}

/* ========================================================================
 * JSON GENERATION
 * ======================================================================== */

/**
 * @brief Build a JSON payload containing networking diagnostics.
 * @return Newly allocated JSON string, or NULL on failure.
 */
char *
networking_get_json(void)
{
	NetworkingSample sample;
	NetworkingSample history[120];
	size_t history_count = 0;
	char *json = NULL;
	size_t json_size = NETWORK_JSON_BUFFER_SIZE;
	size_t offset = 0;
	struct tm tm_buf;
	char timestamp[64];
	struct tm *tm_ptr;

	(void)pthread_once(&g_networking_once, networking_ring_bootstrap);
	if (!networking_ring_last(&g_networking_ring, &sample))
		networking_collect_sample(&sample);
	history_count = networking_ring_last_n(&g_networking_ring,
		sizeof(history) / sizeof(history[0]), history);

	/* Allocate JSON buffer */
	json = malloc(json_size);
	if (!json) {
		LOG("Failed to allocate JSON buffer");
		return NULL;
	}

	/* Timestamp */
	tm_ptr = localtime_r(&sample.ts, &tm_buf);
	if (tm_ptr) {
		strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
				 tm_ptr);
	} else {
		strlcpy(timestamp, "unknown", sizeof(timestamp));
	}

	/* Build JSON */
	offset += snprintf(json + offset, json_size - offset,
					   "{\"timestamp\":\"%s\",\"timestamp_unix\":%lld,",
					   timestamp, (long long)sample.ts);

	/* Routes */
	offset += snprintf(json + offset, json_size - offset, "\"routes\":[");
	for (int i = 0; i < sample.route_count && offset < json_size - 1000; i++) {
		offset +=
		snprintf(json + offset, json_size - offset,
				 "%s{\"destination\":\"%s\",\"gateway\":\"%s\","
				 "\"netmask\":\"%s\",\"interface\":\"%s\","
				 "\"flags\":\"%s\"}",
		   i > 0 ? "," : "", sample.routes[i].destination,
		   sample.routes[i].gateway, sample.routes[i].netmask,
		   sample.routes[i].interface, sample.routes[i].flags_str);
	}
	offset += snprintf(json + offset, json_size - offset, "],");

	/* DNS */
	offset += snprintf(json + offset, json_size - offset,
					   "\"dns\":{\"nameservers\":[");
	for (int i = 0; i < sample.dns.nameserver_count; i++) {
		offset +=
		snprintf(json + offset, json_size - offset, "%s\"%s\"",
				 i > 0 ? "," : "", sample.dns.nameservers[i]);
	}
	offset += snprintf(json + offset, json_size - offset,
					   "],\"domain\":\"%s\",\"search\":\"%s\"},",
					sample.dns.domain, sample.dns.search);

	/* Interface Stats */
	offset +=
	snprintf(json + offset, json_size - offset, "\"interfaces\":[");
	for (int i = 0; i < sample.interface_count && offset < json_size - 1000; i++) {
		offset +=
		snprintf(json + offset, json_size - offset,
				 "%s{\"interface\":\"%s\",\"ipv4\":\"%s\",\"rx_packets\":%llu,"
				 "\"rx_bytes\":%llu,\"rx_errors\":%llu,"
				 "\"tx_packets\":%llu,\"tx_bytes\":%llu,"
				 "\"tx_errors\":%llu}",
		   i > 0 ? "," : "", sample.interfaces[i].interface,
		   sample.interfaces[i].ipv4, sample.interfaces[i].rx_packets,
		   sample.interfaces[i].rx_bytes, sample.interfaces[i].rx_errors,
		   sample.interfaces[i].tx_packets, sample.interfaces[i].tx_bytes,
		   sample.interfaces[i].tx_errors);
	}
	offset += snprintf(json + offset, json_size - offset, "],");

	/* Ring history */
	offset += snprintf(json + offset, json_size - offset, "\"history\":[");
	for (size_t i = 0; i < history_count && offset < json_size - 2000; i++) {
		offset += snprintf(json + offset, json_size - offset,
			"%s{\"ts\":%lld,\"interfaces\":[",
			i > 0 ? "," : "", (long long)history[i].ts);

		for (int j = 0; j < history[i].interface_count && offset < json_size - 1000; j++) {
			offset += snprintf(json + offset, json_size - offset,
				"%s{\"interface\":\"%s\",\"ipv4\":\"%s\",\"rx_bytes\":%llu,\"tx_bytes\":%llu}",
				j > 0 ? "," : "", history[i].interfaces[j].interface,
				history[i].interfaces[j].ipv4,
				history[i].interfaces[j].rx_bytes,
				history[i].interfaces[j].tx_bytes);
		}

		offset += snprintf(json + offset, json_size - offset, "]}");
	}
	offset += snprintf(json + offset, json_size - offset, "]");

	offset += snprintf(json + offset, json_size - offset, "}");

	return json;
}

/* ========================================================================
 * HTTP HANDLERS
 * ======================================================================== */

/* Handler for the networking dashboard page. */
/**
 * @brief Networking handler.
 * @param req Request context for response generation.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
int
networking_handler(http_request_t *req)
{
	struct template_data data = {
		.title          = "MiniWeb - Network Configuration",
		.page_content   = "networking.html",
		.extra_head_file = "networking_extra_head.html",
		.extra_js_file  = "networking_extra_js.html"
	};
	return http_render_template(req, &data, NULL);
}

/**
 * @brief Networking api handler.
 * @param req Request context for response generation.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
int
networking_api_handler(http_request_t *req)
{
	char *json = networking_get_json();
	if (!json) {
		return http_send_error(req, 500, "Network data collection failed");
	}

	http_response_t *resp = http_response_create();
	resp->status_code = 200;
	resp->content_type = "application/json";
	http_response_set_body(resp, json, strlen(json), 1);

	int ret = http_response_send(req, resp);
	http_response_free(resp);
	return ret;
}
