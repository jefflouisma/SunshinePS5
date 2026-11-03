#ifndef SUNSHINE_PROTOCOL_AUDIO_H
#define SUNSHINE_PROTOCOL_AUDIO_H
#include "runtime.h"

enum { SP5_AUDIO_PAYLOAD_LIMIT = 1376, SP5_AUDIO_WIRE_LIMIT = 1400 };
typedef struct {
    uint32_t sample_rate;
    uint8_t channels, duration_ms;
    uint16_t opus_size, first_sequence;
    uint32_t first_timestamp_ms, key_id;
    bool encrypted;
    uint8_t key[16];
} sp5_audio_config;
typedef struct sp5_audio sp5_audio;
/* Setup only; the encoder owner supplies genuine CBR stereo Opus at 48kHz,
 * 5/10ms. This packetizer does not capture, encode, resample or prove audibility.
 * First sequence must be 4-aligned. Fixed payload size persists for the session. */
sp5_audio *sp5_audio_create(const sp5_audio_config *config);
void sp5_audio_destroy(sp5_audio *audio);
/* No allocation/I/O. One submitted packet must be drained before the next;
 * each fourth submission additionally emits 2 audio FEC shards. Encryption
 * failure makes this instance unusable; callers end the session. */
sp5_result sp5_audio_submit(sp5_audio *audio, sp5_bytes opus);
/* Buffer-capacity failure preserves output; idle returns WOULD_BLOCK. */
sp5_result sp5_audio_next(sp5_audio *audio, sp5_buffer *wire);
#endif
