#ifndef SUNSHINE_PROTOCOL_RTSP_H
#define SUNSHINE_PROTOCOL_RTSP_H
#include "protocol/http.h"
#include "protocol/negotiation.h"

enum { SP5_RTSP_LIMIT = 18432 };
typedef struct {
    sp5_host *host;
    sp5_devices *devices;
    sp5_session *session;
    sp5_http_launch launch;
    sp5_bytes verified_certificate;
    const char *peer;
    bool on_link;
    uint16_t audio_port, video_port, control_port;
} sp5_rtsp_config;
typedef struct sp5_rtsp sp5_rtsp;
/* Setup-only allocation. Serialized negotiation owner; request state is fixed.
 * The adapter supplies the authenticated launch's ACTUAL peer and canonical
 * certificate, never an arbitrary identity supplied by an RTSP client. */
sp5_rtsp *sp5_rtsp_create(const sp5_rtsp_config *config);
void sp5_rtsp_destroy(sp5_rtsp *service);
/* One complete request/connection. Encrypted launch enforces GCM in both
 * directions, independent counters, replay rejection. Plain launch is bound to
 * the authenticated launch peer + registry lease. Output is complete RTSP wire.
 * ANNOUNCE configures; the lifecycle owner must reserve/start resources before
 * activating. PLAY succeeds only for that activated session. */
sp5_result sp5_rtsp_exchange(sp5_rtsp *service, const char *peer, bool on_link, sp5_bytes wire,
                             uint64_t now_us, sp5_optional_u64 wall_us, sp5_buffer *response);
/* Serialized negotiation-owner accessor: true only after successful PLAY. */
bool sp5_rtsp_playing(const sp5_rtsp *service);
sp5_result sp5_rtsp_negotiated(const sp5_rtsp *service, sp5_negotiated *chosen);
#endif
