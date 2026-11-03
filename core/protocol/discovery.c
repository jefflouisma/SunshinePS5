#include "discovery.h"

#include "protocol.h"

#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <arpa/inet.h>
#include <dns_sd.h>
#endif

sp5_result sp5_discovery_init(sp5_discovery *discovery, sp5_discovery_ops ops)
{
    if (discovery == NULL || ops.publish == NULL || ops.withdraw == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *discovery = (sp5_discovery){.ops = ops};
    return sp5_success();
}

sp5_result sp5_discovery_stop(sp5_discovery *discovery)
{
    if (discovery == NULL || discovery->ops.withdraw == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (!discovery->published) {
        return sp5_success();
    }
    sp5_result result = discovery->ops.withdraw(discovery->ops.context);
    if (result.status == SP5_OK) {
        discovery->published = false;
    }
    return result;
}

sp5_result sp5_discovery_refresh(sp5_discovery *discovery, const sp5_host *host,
                                 uint32_t interface_index, uint16_t http_port)
{
    if (discovery == NULL || host == NULL || discovery->ops.publish == NULL ||
        interface_index == 0 || http_port == 0 || !sp5_settings_valid(&host->settings) ||
        memchr(host->address, 0, sizeof(host->address)) == NULL ||
        !sp5_peer_permitted(host->address, true)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (host->state == SP5_HOST_STOPPING) {
        return sp5_discovery_stop(discovery);
    }
    if (discovery->published && strcmp(discovery->record.name, host->settings.host_name) == 0 &&
        strcmp(discovery->record.address, host->address) == 0 &&
        discovery->record.port == http_port &&
        discovery->record.interface_index == interface_index) {
        return sp5_success();
    }
    sp5_result result = sp5_discovery_stop(discovery);
    if (result.status != SP5_OK) {
        return result;
    }
    sp5_discovery_record next = {.port = http_port, .interface_index = interface_index};
    memcpy(next.name, host->settings.host_name, strlen(host->settings.host_name) + 1);
    memcpy(next.address, host->address, strlen(host->address) + 1);
    result = discovery->ops.publish(discovery->ops.context, &next);
    if (result.status == SP5_OK) {
        discovery->record = next;
        discovery->published = true;
    }
    return result;
}

sp5_result sp5_discovery_manual(sp5_bytes text, bool on_link, sp5_manual_endpoint *endpoint)
{
    if (endpoint == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *endpoint = (sp5_manual_endpoint){0};
    if (text.data == NULL || text.size == 0 || text.size >= 140) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    char input[140];
    memcpy(input, text.data, text.size);
    input[text.size] = 0;
    if (memchr(input, 0, text.size) != NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    char *address = input;
    const char *port = NULL;
    if (input[0] == '[') {
        address = input + 1;
        char *end = strchr(address, ']');
        if (end == NULL || (end[1] != 0 && end[1] != ':')) {
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
        if (end[1] == ':') {
            port = end + 2;
        }
        *end = 0;
    } else {
        char *colon = strchr(address, ':');
        if (colon != NULL && strchr(colon + 1, ':') == NULL) {
            *colon = 0;
            port = colon + 1;
        }
    }
    /* Default HTTP port: protocol-notes.md port map; Sunshine d0b4a88
     * src/nvhttp.h:48 and src/config.cpp:905. No discovery dependency. */
    uint32_t number = 47989;
    if (port != NULL) {
        number = 0;
        if (*port == 0) {
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
        for (; *port != 0; ++port) {
            if (*port < '0' || *port > '9' || number > (65535U - (unsigned)(*port - '0')) / 10U) {
                return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
            }
            number = number * 10U + (unsigned)(*port - '0');
        }
    }
    if (number == 0 || strlen(address) >= sizeof(endpoint->address) ||
        !sp5_peer_permitted(address, on_link)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    memcpy(endpoint->address, address, strlen(address) + 1);
    endpoint->port = (uint16_t)number;
    return sp5_success();
}

struct sp5_dnssd {
#ifdef __APPLE__
    DNSServiceRef service;
#endif
    sp5_reason diagnostic;
};

#ifdef __APPLE__
static void registered(DNSServiceRef service, DNSServiceFlags flags, DNSServiceErrorType error,
                       const char *name, const char *type, const char *domain, void *context)
{
    (void)service;
    /* Capture the native callback as one registration result. Its parameter
     * order is imposed by DNSServiceRegisterReply, not a core API choice. */
    const struct {
        DNSServiceFlags flags;
        DNSServiceErrorType error;
        const char *name, *type, *domain;
    } reply = {flags, error, name, type, domain};
    sp5_dnssd *adapter = context;
    if (reply.error != kDNSServiceErr_NoError) {
        adapter->diagnostic =
            reply.error == kDNSServiceErr_NameConflict ? SP5_REASON_CONFLICT : SP5_REASON_PLATFORM;
    } else if (reply.name == NULL || reply.type == NULL || reply.domain == NULL ||
               strcmp(reply.type, "_nvstream._tcp.") != 0 || strcmp(reply.domain, "local.") != 0) {
        adapter->diagnostic = SP5_REASON_PLATFORM;
    }
}

#endif

static sp5_result native_withdraw(void *context)
{
    sp5_dnssd *adapter = context;
    if (adapter == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
#ifdef __APPLE__
    if (adapter->service != NULL) {
        DNSServiceRefDeallocate(adapter->service);
        adapter->service = NULL;
    }
#endif
    return sp5_success();
}

static sp5_result native_publish(void *context, const sp5_discovery_record *record)
{
    sp5_dnssd *adapter = context;
    if (adapter == NULL || record == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
#ifdef __APPLE__
    if (adapter->service != NULL) {
        return sp5_failure(SP5_REASON_CONFLICT);
    }
    adapter->diagnostic = SP5_REASON_NONE;
    /* Moonlight Qt 8f2e52e54046fc942e2b80ffb88a71ff0f2300bd
     * app/backend/computermanager.cpp:372 browses _nvstream._tcp.local.
     * DNS-SD daemon implements RFC 6762 §§8–11 and RFC 6763 SRV/TXT.
     * Explicit local domain/interface; no wide-area DNS or auto-renaming. */
    DNSServiceErrorType error = DNSServiceRegister(
        &adapter->service, kDNSServiceFlagsNoAutoRename, record->interface_index, record->name,
        "_nvstream._tcp", "local.", NULL, htons(record->port), 0, NULL, registered, adapter);
    return error == kDNSServiceErr_NoError ? sp5_success() : sp5_failure(SP5_REASON_PLATFORM);
#else
    return sp5_failure(SP5_REASON_UNSUPPORTED);
#endif
}

sp5_dnssd *sp5_dnssd_create(void)
{
    return calloc(1, sizeof(sp5_dnssd));
}

void sp5_dnssd_destroy(sp5_dnssd *adapter)
{
    if (adapter != NULL) {
        (void)native_withdraw(adapter);
        free(adapter);
    }
}

sp5_discovery_ops sp5_dnssd_operations(sp5_dnssd *adapter)
{
    return (sp5_discovery_ops){adapter, native_publish, native_withdraw};
}

int sp5_dnssd_fd(const sp5_dnssd *adapter)
{
#ifdef __APPLE__
    if (adapter != NULL && adapter->service != NULL) {
        return DNSServiceRefSockFD(adapter->service);
    }
#else
    (void)adapter;
#endif
    return -1;
}

sp5_result sp5_dnssd_process(sp5_dnssd *adapter)
{
    if (adapter == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
#ifdef __APPLE__
    if (adapter->service != NULL &&
        DNSServiceProcessResult(adapter->service) != kDNSServiceErr_NoError) {
        adapter->diagnostic = SP5_REASON_PLATFORM;
    }
#else
    return sp5_failure(SP5_REASON_UNSUPPORTED);
#endif
    return adapter->diagnostic == SP5_REASON_NONE ? sp5_success()
                                                  : sp5_failure(adapter->diagnostic);
}
