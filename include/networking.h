#ifndef NETWORKING_H
#define NETWORKING_H

#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include "http_handler.h"

/* --- Data Structures --- */

typedef struct {
    char destination[64];
    char gateway[64];
    char netmask[64];
    char interface[IFNAMSIZ];
    int flags;
    char flags_str[32];
} RouteEntry;

typedef struct {
    char nameservers[8][46]; /* IPv4/IPv6 nameserver strings */
    int nameserver_count;
    char domain[256];
    char search[512];
} DnsConfig;

typedef struct {
    char interface[IFNAMSIZ];
    unsigned long long rx_packets;
    unsigned long long rx_bytes;
    unsigned long long rx_errors;
    unsigned long long rx_dropped;
    unsigned long long tx_packets;
    unsigned long long tx_bytes;
    unsigned long long tx_errors;
    unsigned long long tx_dropped;
} NetStats;

typedef struct {
    char protocol[8];
    char local_addr[64];
    int local_port;
    char remote_addr[64];
    int remote_port;
    char state[16];
} NetworkConnection;

/* --- Core Collection API --- */

/** Collect kernel routing entries. */
int networking_get_routes(RouteEntry *routes, int max_routes);

/** Collect DNS resolver configuration. */
int networking_get_dns_config(DnsConfig *config);

/** Collect per-interface packet and byte counters. */
int networking_get_if_stats(NetStats *stats, int max_interfaces);

/** Collect active TCP/UDP socket connection entries. */
int networking_get_connections(NetworkConnection *conns, int max_conns);

/** Build a JSON payload with networking diagnostics. */
char *networking_get_json(void);

/* --- HTTP Handlers --- */

/**
 * Render the networking HTML page.
 *
 * @param req Request context.
 * @return HTTP send result code.
 */
int networking_handler(http_request_t *req);

/**
 * Serve networking data as JSON.
 *
 * @param req Request context.
 * @return HTTP send result code.
 */
int networking_api_handler(http_request_t *req);

/* Optional compatibility alias. */
#define networking_page_handler networking_handler

#endif /* NETWORKING_H */
