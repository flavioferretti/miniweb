#ifndef NETWORKING_H
#define NETWORKING_H

#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include "http_handler.h"

/* --- Strutture Dati --- */

/* Route entry */
typedef struct {
    char destination[64];
    char gateway[64];
    char netmask[64];
    char interface[IFNAMSIZ];
    int flags;
    char flags_str[32];
} RouteEntry;

/* DNS configuration */
typedef struct {
    char nameservers[8][46]; /* Array di stringhe per IPv4/IPv6 */
    int nameserver_count;
    char domain[256];
    char search[512];
} DnsConfig;

/* Network statistics */
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

/* Network connections (TCP/UDP) */
typedef struct {
    char protocol[8];
    char local_addr[64];
    int local_port;
    char remote_addr[64];
    int remote_port;
    char state[16];
} NetworkConnection;

/* --- Prototipi Funzioni Core --- */

int networking_get_routes(RouteEntry *routes, int max_routes);
int networking_get_dns_config(DnsConfig *config);
int networking_get_if_stats(NetStats *stats, int max_interfaces);
int networking_get_connections(NetworkConnection *conns, int max_conns);
char *networking_get_json(void);

/* --- HTTP Handlers (Nuova Interfaccia kqueue) --- */

/**
 * Renderizza la pagina HTML del networking.
 * Usata in urls.c come 'networking_handler'
 */
int networking_handler(http_request_t *req);

/**
 * Gestisce le richieste API JSON.
 */
int networking_api_handler(http_request_t *req);

/* Alias per compatibilità se necessario (opzionale) */
#define networking_page_handler networking_handler

#endif /* NETWORKING_H */
