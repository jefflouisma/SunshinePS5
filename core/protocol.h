#ifndef SUNSHINE_CORE_PROTOCOL_H
#define SUNSHINE_CORE_PROTOCOL_H

#include "runtime.h"

#ifndef SP5_CORE_API
#define SP5_CORE_API
#endif

/* Byte spans are borrowed, parser outputs never outlive
 * their request. Limits are caller-supplied pool bounds, not wire constants. */
typedef struct {
    sp5_bytes method;
    sp5_bytes target;
    sp5_bytes body;
    uint32_t cseq;
} sp5_request;
SP5_CORE_API sp5_result sp5_http_parse(sp5_bytes wire, size_t limit, sp5_request *request);
SP5_CORE_API sp5_result sp5_rtsp_parse(sp5_bytes wire, size_t limit, sp5_request *request);
/* peer is the actual transport address; on_link comes from the transport's
 * receiving interface, never from a forwarded header or client assertion. */
SP5_CORE_API bool sp5_peer_permitted(const char *peer, bool on_link);

typedef enum { SP5_BITRATE_NONE, SP5_BITRATE_RAISED, SP5_BITRATE_LOWERED } sp5_bitrate_adjustment;
typedef struct {
    uint32_t requested_kbps;
    uint32_t chosen_kbps;
    sp5_bitrate_adjustment adjustment;
} sp5_bitrate;
/* Required target is a decimal positive uint32; min/max describe a synthetic
 * validated mode in tests. No numeric production capability is implied. */
SP5_CORE_API sp5_result sp5_negotiate_bitrate(sp5_bytes sdp, uint32_t minimum_kbps,
                                              uint32_t maximum_kbps, sp5_bitrate *bitrate);

typedef struct sp5_devices sp5_devices;
typedef struct sp5_authority sp5_authority;
typedef struct sp5_session sp5_session;
typedef enum {
    SP5_SESSION_NEGOTIATING,
    SP5_SESSION_ACTIVE,
    SP5_SESSION_ENDING,
    SP5_SESSION_ENDED
} sp5_session_state;
typedef enum {
    SP5_END_CLIENT_QUIT,
    SP5_END_CLIENT_LOST,
    SP5_END_REVOKED,
    SP5_END_HOST_STOPPING,
    SP5_END_CONSOLE_CONFLICT,
    SP5_END_ERROR
} sp5_end_reason;
enum { SP5_SESSION_CONTROLLER_CAPACITY = 4 };
typedef struct {
    uint32_t id;
    uint64_t connection_order;
    bool connected;
    sp5_controller_state state;
    uint64_t delivered_count, failed_count;
} sp5_session_controller;
typedef struct {
    uint64_t id;
    sp5_session_state state;
    bool has_end_reason;
    sp5_end_reason end_reason;
    sp5_reason diagnostic;
    char device_id[65];
    char device_name[64];
    uint64_t started_us;
    sp5_optional_u64 started_wall_unix_us;
    bool mode_configured;
    uint32_t width, height, fps;
    char codec[24];
    sp5_dynamic_range dynamic_range;
    uint64_t dynamic_range_changed_us;
    uint64_t dynamic_range_sequence;
    uint32_t requested_bitrate_kbps, chosen_bitrate_kbps;
    sp5_bitrate_adjustment bitrate_adjustment;
    sp5_optional_u64 measured_bitrate_bps;
    uint64_t measured_interval_start_us, measured_interval_end_us;
    sp5_session_controller controllers[SP5_SESSION_CONTROLLER_CAPACITY];
    size_t controller_count;
} sp5_session_view;
/* Create/destroy are setup-only. Certificate bytes, not display IDs, authorize.
 * Revocation invalidates already admitted negotiations and subsequent input. */
SP5_CORE_API sp5_authority *sp5_authority_create(sp5_devices *registry);
SP5_CORE_API void sp5_authority_destroy(sp5_authority *authority);
SP5_CORE_API sp5_result sp5_authority_pair(sp5_authority *authority, sp5_bytes certificate);
SP5_CORE_API sp5_result sp5_authority_revoke(sp5_authority *authority, sp5_bytes certificate);
SP5_CORE_API sp5_session *sp5_session_create(sp5_authority *authority);
SP5_CORE_API void sp5_session_destroy(sp5_session *session);
SP5_CORE_API sp5_result sp5_session_admit(sp5_session *session, sp5_bytes certificate,
                                          uint64_t session_id);
SP5_CORE_API sp5_result sp5_session_activate(sp5_session *session);
SP5_CORE_API bool sp5_session_authorized(const sp5_session *session, sp5_bytes certificate);
SP5_CORE_API sp5_result sp5_session_end(sp5_session *session, sp5_end_reason reason);
SP5_CORE_API sp5_session_view sp5_session_read(const sp5_session *session);

#endif
