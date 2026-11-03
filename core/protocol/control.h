#ifndef SUNSHINE_PROTOCOL_CONTROL_H
#define SUNSHINE_PROTOCOL_CONTROL_H
#include "protocol/rtsp.h"

enum {
    SP5_CONTROL_PAYLOAD_LIMIT = 1024,
    SP5_CONTROL_WIRE_LIMIT = SP5_CONTROL_PAYLOAD_LIMIT + 28,
    SP5_CONTROL_QUEUE_CAPACITY = 32,
    SP5_CONTROL_CHANNEL_COUNT = 48
};
typedef enum {
    SP5_CONTROL_RELIABLE,
    SP5_CONTROL_UNRELIABLE,
    SP5_CONTROL_UNSEQUENCED
} sp5_control_delivery;
typedef struct {
    uint16_t type;
    uint8_t channel;
    sp5_control_delivery delivery;
    uint32_t sequence;
    uint64_t received_us;
    size_t payload_size;
    uint8_t payload[SP5_CONTROL_PAYLOAD_LIMIT];
} sp5_control_event;
typedef struct {
    uint32_t frame_index;
    uint16_t highest_sequence, next_contiguous_sequence, missing_before_highest;
    uint16_t data_total, parity_total, data_received, parity_received;
    uint8_t fec_percent, block_index, block_count;
    /* These are CLIENT reports, not measured host loss or confirmed recovery. */
} sp5_control_fec;
sp5_result sp5_control_fec_read(const sp5_control_event *event, sp5_control_fec *feedback);

typedef struct {
    sp5_session *session;
    sp5_bytes verified_certificate;
    sp5_http_launch launch;
    sp5_negotiated negotiated;
    const char *peer;
    bool on_link;
    uint64_t started_us;
    void *end_context;
    /* Mandatory independent input-first termination hook. It runs once AFTER
     * input admission is invalidated, BEFORE network teardown. The lifecycle
     * owner supplies controller neutralization/removal, never media draining. */
    sp5_result (*release_input)(void *context, sp5_end_reason reason);
} sp5_control_config;
typedef struct sp5_control sp5_control;
sp5_control *sp5_control_create(const sp5_control_config *config);
void sp5_control_destroy(sp5_control *control);
/* Trusted transport-adapter entrypoints, like HTTP's actual-peer parameter:
 * never derive peer/on_link from an application payload. Accept binds the
 * launch's token/address, while each received packet proves the launch key. */
sp5_result sp5_control_accept(sp5_control *control, const char *peer, bool on_link,
                              uint32_t connect_data, uint64_t now_us);
sp5_result sp5_control_receive(sp5_control *control, const char *peer, bool on_link,
                               uint8_t channel, sp5_control_delivery delivery, sp5_bytes wire,
                               uint64_t now_us);
/* An event is data, not a lasting authorization token. Input consumers recheck
 * session authorization immediately before platform delivery (T062). */
sp5_result sp5_control_take(sp5_control *control, sp5_control_event *event);
sp5_result sp5_control_tick(sp5_control *control, uint64_t now_us);
sp5_result sp5_control_end(sp5_control *control, sp5_end_reason reason);
/* Host feedback is reliable channel zero (notes §11). Queueing never sends or
 * waits on media. Only defined host-to-client types are accepted. */
sp5_result sp5_control_send(sp5_control *control, uint16_t type, sp5_bytes payload);
sp5_result sp5_control_take_wire(sp5_control *control, sp5_buffer *wire);
sp5_reason sp5_control_diagnostic(const sp5_control *control);

/* ENet callbacks are process-global. One explicit setup-only runtime owns their
 * fixed pools; never mix unrelated ENet users, threads or allocator domains.
 * All transport access is serialized on the independent control/input owner. */
typedef struct sp5_control_runtime sp5_control_runtime;
sp5_control_runtime *sp5_control_runtime_create(void);
sp5_result sp5_control_runtime_destroy(sp5_control_runtime *runtime);
typedef struct {
    size_t used_blocks, high_water;
    uint64_t exhausted;
} sp5_control_pool_view;
sp5_control_pool_view sp5_control_pool_read(const sp5_control_runtime *runtime);
typedef struct sp5_control_transport sp5_control_transport;
/* Explicit numeric private bind address; port zero requests a local ephemeral
 * port for software fixtures. No resolver, forwarding or router configuration. */
sp5_control_transport *sp5_control_transport_open(sp5_control_runtime *runtime,
                                                  sp5_control *control, const char *bind_address,
                                                  uint16_t port);
uint16_t sp5_control_transport_port(const sp5_control_transport *transport);
/* Nonblocking bounded service step; caller provides its monotonic clock. ENet
 * handles reliability and its 500ms idle PING; clients send 100ms app pings.
 * Ten-second contact deadline counts authenticated application contact only. */
sp5_result sp5_control_transport_step(sp5_control_transport *transport, uint64_t now_us);
void sp5_control_transport_close(sp5_control_transport *transport);
#endif
