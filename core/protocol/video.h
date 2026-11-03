#ifndef SUNSHINE_PROTOCOL_VIDEO_H
#define SUNSHINE_PROTOCOL_VIDEO_H
#include "runtime.h"

enum { SP5_VIDEO_PACKET_LIMIT = 2048, SP5_VIDEO_WIRE_LIMIT = SP5_VIDEO_PACKET_LIMIT + 48 };
typedef struct {
    uint16_t packet_size;
    uint8_t fec_percent, minimum_parity;
    bool encrypted;
    uint8_t key[16];
    uint32_t first_packet_index;
} sp5_video_config;
typedef struct {
    sp5_bytes bitstream;
    uint32_t frame_index, rtp_timestamp;
    bool key_frame, after_invalidation, repeated;
    /* Capture-to-PRE-packetization duration supplied by the timing owner, even
     * with detailed metrics off. Missing/repeated => zero; never final-send time.
     * RTP timestamp is independently mapped to the protocol's 90kHz clock. */
    sp5_optional_u64 processing_us;
} sp5_video_frame_input;
typedef struct sp5_video sp5_video;
/* Setup-only allocation: one 255-shard pool, warmed OpenSSL context. Local
 * bounds: packet 64..2048, FEC 0..100%, minimum parity 0..2, at most 4 blocks.
 * These are software limits, not measured PS5 capabilities. */
sp5_video *sp5_video_create(const sp5_video_config *config);
void sp5_video_destroy(sp5_video *video);
uint16_t sp5_video_latency(sp5_optional_u64 elapsed_us, bool repeated);
/* Begin borrows bitstream until the last successful next/cancel. An active frame
 * is never replaced. Oversized frames fail before output; there is no queue. */
sp5_result sp5_video_begin(sp5_video *video, const sp5_video_frame_input *frame);
/* One datagram per call; WOULD_BLOCK when idle. Capacity failures preserve the
 * cursor. There is no I/O: the media owner submits at its designated send point.
 * Crypto failures cancel the frame and never reuse an IV. */
sp5_result sp5_video_next(sp5_video *video, sp5_buffer *wire);
void sp5_video_cancel(sp5_video *video);
#endif
