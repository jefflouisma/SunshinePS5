#ifndef SUNSHINE_PROTOCOL_NEGOTIATION_H
#define SUNSHINE_PROTOCOL_NEGOTIATION_H
#include "session/session.h"

enum { SP5_SDP_LIMIT = 16384, SP5_SDP_ATTRIBUTE_CAPACITY = 64 };
typedef struct {
    size_t mode_index;
    sp5_bitrate bitrate;
    uint32_t encryption_flags, feature_flags, packet_size, audio_duration_ms;
    bool high_quality_audio;
} sp5_negotiated;
/* All numeric attributes are strict uint32; duplicate attributes are rejected.
 * Only caller-supplied capability records qualify modes. No PS5 defaults. */
sp5_result sp5_sdp_negotiate(sp5_bytes sdp, const sp5_host *host, sp5_negotiated *chosen);
sp5_result sp5_sdp_describe(const sp5_host *host, sp5_buffer *sdp);
#endif
