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

/* Include del progetto */
#include "../include/networking.h"
#include "../include/template_engine.h"
#include "../include/config.h"
#include "../include/http_handler.h"

#ifndef SA_SIZE
#define SA_SIZE(sa)                                                   \
(((sa)->sa_len == 0) ? sizeof(struct sockaddr) : (size_t)(sa)->sa_len)
#endif

/* Verbose logging */
extern int config_verbose;
#define LOG(...)                                            \
do {                                                        \
	if (config_verbose) {                                   \
		fprintf(stderr, "[NETWORK] " __VA_ARGS__);          \
		fprintf(stderr, "\n");                              \
	}                                                       \
} while (0)


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

			strlcpy(stats[count].interface, ifa->ifa_name,
					sizeof(stats[count].interface));

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
	char *json = NULL;
	RouteEntry routes[50];
	DnsConfig dns;
	NetStats if_stats[10];
	int route_count, if_count;
	size_t json_size = 65536; /* 64KB */
	size_t offset = 0;
	time_t now;
	struct tm tm_buf;
	char timestamp[64];

	/* Allocate JSON buffer */
	json = malloc(json_size);
	if (!json) {
		LOG("Failed to allocate JSON buffer");
		return NULL;
	}

	/* Timestamp */
	time(&now);
	struct tm *tm_ptr = localtime_r(&now, &tm_buf);
	if (tm_ptr) {
		strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
				 tm_ptr);
	} else {
		strlcpy(timestamp, "unknown", sizeof(timestamp));
	}

	/* Collect data */
	route_count = networking_get_routes(routes, 50);
	networking_get_dns_config(&dns);
	if_count = networking_get_if_stats(if_stats, 10);

	/* Build JSON */
	offset += snprintf(json + offset, json_size - offset,
					   "{\"timestamp\":\"%s\",", timestamp);

	/* Routes */
	offset += snprintf(json + offset, json_size - offset, "\"routes\":[");
	for (int i = 0; i < route_count && offset < json_size - 1000; i++) {
		offset +=
		snprintf(json + offset, json_size - offset,
				 "%s{\"destination\":\"%s\",\"gateway\":\"%s\","
				 "\"netmask\":\"%s\",\"interface\":\"%s\","
				 "\"flags\":\"%s\"}",
		   i > 0 ? "," : "", routes[i].destination,
		   routes[i].gateway, routes[i].netmask,
		   routes[i].interface, routes[i].flags_str);
	}
	offset += snprintf(json + offset, json_size - offset, "],");

	/* DNS */
	offset += snprintf(json + offset, json_size - offset,
					   "\"dns\":{\"nameservers\":[");
	for (int i = 0; i < dns.nameserver_count; i++) {
		offset +=
		snprintf(json + offset, json_size - offset, "%s\"%s\"",
				 i > 0 ? "," : "", dns.nameservers[i]);
	}
	offset += snprintf(json + offset, json_size - offset,
					   "],\"domain\":\"%s\",\"search\":\"%s\"},",
					dns.domain, dns.search);

	/* Interface Stats */
	offset +=
	snprintf(json + offset, json_size - offset, "\"interfaces\":[");
	for (int i = 0; i < if_count && offset < json_size - 1000; i++) {
		offset +=
		snprintf(json + offset, json_size - offset,
				 "%s{\"interface\":\"%s\",\"rx_packets\":%llu,"
				 "\"rx_bytes\":%llu,\"rx_errors\":%llu,"
				 "\"tx_packets\":%llu,\"tx_bytes\":%llu,"
				 "\"tx_errors\":%llu}",
		   i > 0 ? "," : "", if_stats[i].interface,
		   if_stats[i].rx_packets, if_stats[i].rx_bytes,
		   if_stats[i].rx_errors, if_stats[i].tx_packets,
		   if_stats[i].tx_bytes, if_stats[i].tx_errors);
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
