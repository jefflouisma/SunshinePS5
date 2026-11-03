#ifndef SUNSHINE_PLATFORM_PS5_ANNEXB_H
#define SUNSHINE_PLATFORM_PS5_ANNEXB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded Annex-B classification adapted from tools/ps5-probes/capture_encode.c
 * (video_start_code / video_record / video_collect_parameters). NAL type maps:
 * FFmpeg n8.0 libavcodec/h264.h and libavcodec/hevc/hevc.h, H.264/H.265 Table 7-1.
 * This validates framing only. It does not decode a picture or prove encoder
 * acceleration, profile, bit depth or source HDR. */

enum { SP5_PS5_AVC_CODEC = 0, SP5_PS5_HEVC_CODEC = 1 };

typedef enum {
    SP5_PS5_ANNEXB_INVALID = 0,
    SP5_PS5_ANNEXB_NONKEY,
    SP5_PS5_ANNEXB_KEY
} sp5_ps5_annexb_class;

static inline size_t sp5_ps5_start_code(const uint8_t *bytes, size_t size, size_t offset)
{
    if (bytes == NULL || offset > size || size - offset < 3 || bytes[offset] != 0 ||
        bytes[offset + 1] != 0)
        return 0;
    if (bytes[offset + 2] == 1)
        return 3;
    return size - offset >= 4 && bytes[offset + 2] == 0 && bytes[offset + 3] == 1 ? 4 : 0;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static inline bool sp5_ps5_annexb_nal_is_key(uint32_t codec, unsigned type)
{
    if (codec == SP5_PS5_AVC_CODEC)
        return type == 5; /* IDR slice. */
    if (codec == SP5_PS5_HEVC_CODEC)
        return type >= 16 && type <= 21; /* BLA_W_LP .. CRA_NUT (IRAP). */
    return false;
}

/* Requires the record to begin with a start code, as the probe did for every
 * retained read record. Returns INVALID on malformed framing or empty input. */
static inline sp5_ps5_annexb_class sp5_ps5_annexb_classify(const uint8_t *bytes, size_t size,
                                                           uint32_t codec)
{
    if (bytes == NULL || size == 0 || codec > SP5_PS5_HEVC_CODEC ||
        sp5_ps5_start_code(bytes, size, 0) == 0)
        return SP5_PS5_ANNEXB_INVALID;
    bool saw_nal = false;
    bool key = false;
    size_t offset = 0;
    while (offset < size) {
        size_t prefix = sp5_ps5_start_code(bytes, size, offset);
        if (prefix == 0) {
            ++offset;
            continue;
        }
        size_t payload = offset + prefix;
        size_t header = codec == SP5_PS5_AVC_CODEC ? 1 : 2;
        if (size - payload < header)
            return SP5_PS5_ANNEXB_INVALID;
        unsigned type;
        if (codec == SP5_PS5_AVC_CODEC) {
            if ((bytes[payload] & 0x80u) != 0 || (bytes[payload] & 31u) == 0)
                return SP5_PS5_ANNEXB_INVALID;
            type = bytes[payload] & 31u;
        } else {
            if ((bytes[payload] & 0x80u) != 0 || (bytes[payload + 1] & 7u) == 0)
                return SP5_PS5_ANNEXB_INVALID;
            type = (bytes[payload] >> 1) & 63u;
        }
        saw_nal = true;
        key = key || sp5_ps5_annexb_nal_is_key(codec, type);
        offset = payload + header;
        while (offset < size && sp5_ps5_start_code(bytes, size, offset) == 0)
            ++offset;
    }
    if (!saw_nal)
        return SP5_PS5_ANNEXB_INVALID;
    return key ? SP5_PS5_ANNEXB_KEY : SP5_PS5_ANNEXB_NONKEY;
}

#endif
