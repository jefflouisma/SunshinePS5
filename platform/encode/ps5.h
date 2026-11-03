#ifndef SUNSHINE_PLATFORM_ENCODE_PS5_H
#define SUNSHINE_PLATFORM_ENCODE_PS5_H

#include "platform.h"

#include <stdint.h>

/* Provisional production encode adapter for the sourced Venc route.
 *
 * R3 recovered no direct normal-process Venc client contract. The one observed
 * working route is Avcap2 -> SceAvCapture daemon -> Venc, with completed
 * Annex-B access units returned by Avcap2 ReadVideo (see research-r3 findings,
 * T041). This adapter therefore accepts only the capture adapter's validated
 * hardware-produced bitstream record and copies it into caller storage. It never
 * runs a CPU encoder, never retains the frame after return, never allocates and
 * never retries. A future direct Venc ABI must replace this boundary only after
 * its declarations and ownership are sourced and device-observed. */

typedef struct {
    bool opened;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t codec;
    uint32_t depth;
    uint32_t chosen_bitrate_kbps;
    sp5_dynamic_range dynamic_range;
    uint64_t encoded_frames;
    sp5_reason failure;
} sp5_ps5_encoder;

void sp5_ps5_encoder_init(sp5_ps5_encoder *encoder);
sp5_encode_ops sp5_ps5_encoder_operations(sp5_ps5_encoder *encoder);
void sp5_ps5_encoder_provenance(const sp5_ps5_encoder *encoder,
                                sp5_platform_provenance *provenance);

#endif
