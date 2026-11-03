#ifndef SUNSHINE_PROTOCOL_DISCOVERY_H
#define SUNSHINE_PROTOCOL_DISCOVERY_H

#include "settings/host.h"

typedef struct {
    char name[SP5_NAME_CAPACITY];
    char address[128];
    uint16_t port;
    uint32_t interface_index;
} sp5_discovery_record;
/* DNS-SD registration owns local-link probing, conflict detection, TTLs and
 * goodbye records. An explicit interface is required; never publish on all
 * interfaces or create forwarding/router configuration. Management owner only. */
typedef struct {
    void *context;
    sp5_result (*publish)(void *context, const sp5_discovery_record *record);
    sp5_result (*withdraw)(void *context);
} sp5_discovery_ops;
typedef struct {
    sp5_discovery_ops ops;
    sp5_discovery_record record;
    bool published;
} sp5_discovery;
sp5_result sp5_discovery_init(sp5_discovery *discovery, sp5_discovery_ops ops);
sp5_result sp5_discovery_refresh(sp5_discovery *discovery, const sp5_host *host,
                                 uint32_t interface_index, uint16_t http_port);
sp5_result sp5_discovery_stop(sp5_discovery *discovery);
/* Numeric private manual endpoints are independent of multicast reachability.
 * IPv6 ports use brackets; scoped link-local addresses use an actual interface
 * at the socket adapter. This parser never resolves DNS or changes routing. */
typedef struct {
    char address[128];
    uint16_t port;
} sp5_manual_endpoint;
sp5_result sp5_discovery_manual(sp5_bytes text, bool on_link, sp5_manual_endpoint *endpoint);
/* Native development-machine DNS-SD adapter. Setup-only allocation; no console
 * adapter is claimed. On platforms without DNS-SD, inject the above operations.
 * Poll fd for readability, then call process; it is a designated wait point. */
typedef struct sp5_dnssd sp5_dnssd;
sp5_dnssd *sp5_dnssd_create(void);
void sp5_dnssd_destroy(sp5_dnssd *adapter);
sp5_discovery_ops sp5_dnssd_operations(sp5_dnssd *adapter);
int sp5_dnssd_fd(const sp5_dnssd *adapter);
sp5_result sp5_dnssd_process(sp5_dnssd *adapter);

#endif
