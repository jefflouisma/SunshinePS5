#ifndef SUNSHINE_PROTOCOL_PAIRING_FLOW_H
#define SUNSHINE_PROTOCOL_PAIRING_FLOW_H

#include "http.h"
#include "pairing_state.h"

typedef struct sp5_pair_flow sp5_pair_flow;
typedef struct {
    sp5_pairing *state;
    sp5_devices *devices;
    sp5_bytes certificate_pem, private_key_pem;
    sp5_system_ops system;
    const char *pin_url;
} sp5_pair_flow_config;
/* Serialized management owner. Setup allocates bounded handshake storage;
 * OpenSSL crypto/storage/notifications stay off input and media producers.
 * State/registry/system context outlive flow; identity and URL are copied. */
sp5_pair_flow *sp5_pair_flow_create(const sp5_pair_flow_config *config);
void sp5_pair_flow_destroy(sp5_pair_flow *flow);
/* Compatible with sp5_http_config.pair. A deferred response retains only a
 * request ID in the transport's fixed connection pool, not borrowed queries.
 * Also handles Moonlight's unauthenticated GET /unpair abort: it ends only the
 * matching pending handshake (counted as a failed PIN once the PIN key is in
 * use), never changes the paired-device registry, and answers an empty 200. */
sp5_result sp5_pair_flow_handle(void *context, const sp5_request *request,
                                sp5_bytes verified_certificate, uint64_t now_us,
                                sp5_optional_u64 wall_us, sp5_http_response *response);
sp5_result sp5_pair_flow_submit_pin(sp5_pair_flow *flow, uint64_t request_id, uint64_t now_us,
                                    sp5_bytes pin, sp5_bytes device_name);
sp5_result sp5_pair_flow_take_certificate(sp5_pair_flow *flow, uint64_t request_id, uint64_t now_us,
                                          sp5_http_response *response);
/* Call from bounded management tick even when a peer abandons its exchange.
 * Backwards clocks fail closed; terminal/expired handshakes erase secrets. */
sp5_result sp5_pair_flow_tick(sp5_pair_flow *flow, uint64_t now_us);
/* Read-only management view: true after a PIN was accepted for this pending
 * request while its Moonlight proof exchange is still running. Exposes no PIN,
 * key or handshake secret; NULL flow or another request ID reads false. */
bool sp5_pair_flow_pin_entered(const sp5_pair_flow *flow, uint64_t request_id);
/* Notification failure is observable independently of successful persistence:
 * a delivered proof must never be reported unpaired after durable acceptance. */
sp5_reason sp5_pair_flow_diagnostic(const sp5_pair_flow *flow);

#endif
