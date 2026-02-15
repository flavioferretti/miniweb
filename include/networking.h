#ifndef NETWORKING_H
#define NETWORKING_H

#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <microhttpd.h>

/* Route entry */
typedef struct {
    char destination[64];
    char gateway[64];
    char netmask[64];
    char interface[IFNAMSIZ];
    int flags;
    char flags_str[32];
} RouteEntry;

/* DNS server */
typedef struct {
    char nameserver[INET6_ADDRSTRLEN];
    int is_ipv6;
} DnsServer;

/* DNS configuration */
typedef struct {
    DnsServer nameservers[8];
    int nameserver_count;
    char domain[256];
    char search[512];
} DnsConfig;

/* Network statistics per interface */
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

/* Network connection (TCP/UDP) */
typedef struct {
    char protocol[8];     /* tcp, udp */
    char local_addr[64];
    int local_port;
    char remote_addr[64];
    int remote_port;
    char state[16];       /* ESTABLISHED, LISTEN, etc */
} NetworkConnection;

/* Function prototypes */
int networking_get_routes(RouteEntry *routes, int max_routes);
int networking_get_dns_config(DnsConfig *config);
int networking_get_if_stats(NetStats *stats, int max_interfaces);
int networking_get_connections(NetworkConnection *conns, int max_conns);
char *networking_get_json(void);

/* HTTP handlers */
int networking_page_handler(void *cls, struct MHD_Connection *connection,
                            const char *url, const char *method,
                            const char *version, const char *upload_data,
                            size_t *upload_data_size, void **con_cls);

int networking_api_handler(void *cls, struct MHD_Connection *connection,
                           const char *url, const char *method,
                           const char *version, const char *upload_data,
                           size_t *upload_data_size, void **con_cls);

#endif /* NETWORKING_H */
