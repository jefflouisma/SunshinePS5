#ifndef SUNSHINE_PROTOCOL_FEC_H
#define SUNSHINE_PROTOCOL_FEC_H
#include "runtime.h"

enum { SP5_FEC_SHARDS = 255, SP5_FEC_SIZE_LIMIT = 65536 };
typedef struct {
    uint8_t logarithm[256], exponent[512];
} sp5_fec;
/* Setup-only table construction. Encoding uses caller-owned nonoverlapping shards,
 * data first then parity, all of exactly size bytes; no allocation or decoding. */
void sp5_fec_init(sp5_fec *fec);
sp5_result sp5_fec_encode(const sp5_fec *fec, uint8_t *const shards[], size_t data, size_t parity,
                          size_t size);
/* Audio's fixed 4+2 matrix differs from the video Cauchy matrix (notes §8). */
sp5_result sp5_fec_audio(const sp5_fec *fec, uint8_t *const shards[6], size_t size);
#endif
