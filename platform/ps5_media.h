#ifndef SUNSHINE_PLATFORM_PS5_MEDIA_H
#define SUNSHINE_PLATFORM_PS5_MEDIA_H

#include "platform.h"

#include <string.h>

/* Provisional adapter-internal media helpers. Nothing here is an SDK ABI.
 * Geometry, codec and rate records are adapted from:
 *   specs/001-moonlight-streaming-host/research-r3-media-re.md
 *     "Completion follow-up: recovered Venc rate and depth controls"
 * and the request transcription in tools/ps5-probes/capture_encode.c.
 * The AVC ceilings are static codec/preset bounds, NOT measured device limits. */

enum { SP5_PS5_CODEC_AVC = 0, SP5_PS5_CODEC_HEVC = 1, SP5_PS5_DEPTH_8 = 8, SP5_PS5_DEPTH_10 = 10 };

typedef struct {
    uint32_t tier;         /* Sony tier 3 = 720p, 4 = 1080p. */
    uint32_t fps;          /* Only the sourced 30/60 request values. */
    uint32_t codec;        /* SP5_PS5_CODEC_*. */
    uint32_t depth;        /* SP5_PS5_DEPTH_*. */
    uint32_t ceiling_kbps; /* 0 means no static ceiling was recovered. */
    bool measured;         /* false: no measured production bitrate population. */
    const char *source_ref;
} sp5_ps5_mode;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static inline bool sp5_ps5_codec_name(const sp5_media_config *config, uint32_t *codec,
                                      uint32_t *depth)
{
    if (config == NULL || config->codec == NULL)
        return false;
    if (strcmp(config->codec, "h264") == 0) {
        *codec = SP5_PS5_CODEC_AVC;
        *depth = SP5_PS5_DEPTH_8;
        return true;
    }
    if (strcmp(config->codec, "hevc") == 0) {
        *codec = SP5_PS5_CODEC_HEVC;
        *depth = SP5_PS5_DEPTH_8;
        return true;
    }
    if (strcmp(config->codec, "hevc-main10") == 0) {
        *codec = SP5_PS5_CODEC_HEVC;
        *depth = SP5_PS5_DEPTH_10;
        return true;
    }
    return false;
}

/* Config-to-request mapping only. The dynamic-range relation is sourced from
 * Sony's request +0x20 depth selector and +0x24 codec selector; Venc rejects
 * ten-bit AVC. SDR/HDR acceptance of the resulting stream remains unverified. */
static inline bool sp5_ps5_mode_validate(const sp5_media_config *config, sp5_ps5_mode *mode)
{
    if (config == NULL || mode == NULL || config->width == 0 || config->height == 0 ||
        config->fps_denominator == 0 || config->chosen_bitrate_kbps == 0 ||
        config->chosen_bitrate_kbps > (uint32_t)(INT32_MAX / 1000))
        return false;
    uint32_t tier;
    if (config->width == 1280 && config->height == 720)
        tier = 3;
    else if (config->width == 1920 && config->height == 1080)
        tier = 4;
    else
        return false;
    if ((uint64_t)config->fps_numerator != (uint64_t)30 * config->fps_denominator &&
        (uint64_t)config->fps_numerator != (uint64_t)60 * config->fps_denominator)
        return false;
    uint32_t fps = config->fps_numerator / config->fps_denominator;
    uint32_t codec, depth;
    if (!sp5_ps5_codec_name(config, &codec, &depth))
        return false;
    /* The recovered Avcap2 request maps codec/depth only; no HDR/PQ request
     * field or conversion was established. Refuse HDR until that mapping is
     * sourced and device-observed. */
    if (config->dynamic_range != SP5_SDR || depth != SP5_PS5_DEPTH_8)
        return false;
    /* Media RE recovered static AVC level ceilings for the four requested modes.
     * HEVC has no recovered ceiling. The cell is a source bound, not a measured
     * minimum/maximum device range; expose it as such. */
    uint32_t ceiling = 0;
    if (codec == SP5_PS5_CODEC_AVC) {
        if (tier == 3 && fps == 30)
            ceiling = 14000;
        else if ((tier == 3 && fps == 60) || (tier == 4 && fps == 30))
            ceiling = 20000;
        else
            ceiling = 50000;
    }
    *mode = (sp5_ps5_mode){.tier = tier,
                           .fps = fps,
                           .codec = codec,
                           .depth = depth,
                           .ceiling_kbps = ceiling,
                           .measured = false,
                           .source_ref = "research-r3-media-re.md#completion-follow-up-recovered-"
                                         "venc-rate-and-depth-controls-2026-10-25"};
    return true;
}

static inline const char *sp5_ps5_format_ref(uint32_t codec, uint32_t depth)
{
    if (codec == SP5_PS5_CODEC_AVC && depth == SP5_PS5_DEPTH_8)
        return "ps5/avcap2/annexb/avc-8-v1";
    if (codec == SP5_PS5_CODEC_HEVC && depth == SP5_PS5_DEPTH_8)
        return "ps5/avcap2/annexb/hevc-8-v1";
    if (codec == SP5_PS5_CODEC_HEVC && depth == SP5_PS5_DEPTH_10)
        return "ps5/avcap2/annexb/hevc-10-v1";
    return NULL;
}

#endif
