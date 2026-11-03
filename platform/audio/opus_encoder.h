#ifndef SUNSHINE_PLATFORM_AUDIO_OPUS_ENCODER_H
#define SUNSHINE_PLATFORM_AUDIO_OPUS_ENCODER_H
#include "platform.h"
/* Setup-only Opus encoder for the provisional console host. The packetizer in
 * core/protocol/audio.c remains responsible for RTP/FEC/encryption. */
typedef struct sp5_opus_encoder sp5_opus_encoder;
sp5_opus_encoder *sp5_opus_encoder_create(uint32_t sample_rate, uint8_t channels,
                                          uint32_t bitrate_bps, uint32_t duration_ms,
                                          sp5_result *result);
void sp5_opus_encoder_destroy(sp5_opus_encoder *encoder);
/* Encodes exactly opus_frame_samples() interleaved float samples per call.
 * Output is bounded by capacity; *size is set only on success. */
sp5_result sp5_opus_encoder_encode_float(sp5_opus_encoder *encoder, const float *pcm,
                                         size_t capacity, sp5_buffer *output);
uint32_t sp5_opus_encoder_frame_samples(const sp5_opus_encoder *encoder);
#endif
