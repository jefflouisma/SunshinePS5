#include "negotiation.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    sp5_bytes name, value;
} attribute;
typedef struct {
    attribute values[SP5_SDP_ATTRIBUTE_CAPACITY];
    size_t count;
} attributes;
static bool same(sp5_bytes span, const char *s)
{
    return span.size == strlen(s) && memcmp(span.data, s, span.size) == 0;
}
static sp5_result parse(sp5_bytes sdp, attributes *attrs)
{
    *attrs = (attributes){0};
    if (sdp.data == NULL || sdp.size == 0 || sdp.size > SP5_SDP_LIMIT)
        return sp5_failure(SP5_REASON_CAPACITY);
    size_t offset = 0;
    while (offset < sdp.size) {
        size_t start = offset;
        while (offset < sdp.size && sdp.data[offset] != '\r' && sdp.data[offset] != '\n') {
            if (sdp.data[offset] < 32 || sdp.data[offset] == 127)
                return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
            ++offset;
        }
        size_t end = offset;
        if (offset < sdp.size) {
            if (sdp.data[offset] != '\r' || offset + 1 >= sdp.size || sdp.data[offset + 1] != '\n')
                return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
            offset += 2;
        }
        if (end - start < 2 || sdp.data[start + 1] != '=')
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        if (sdp.data[start] != 'a')
            continue;
        size_t colon = start + 2;
        while (colon < end && sdp.data[colon] != ':') {
            if (sdp.data[colon] <= 32 || sdp.data[colon] >= 127)
                return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
            ++colon;
        }
        if (colon == start + 2 || colon == end || attrs->count == SP5_SDP_ATTRIBUTE_CAPACITY)
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        sp5_bytes name = {sdp.data + start + 2, colon - start - 2};
        for (size_t i = 0; i < attrs->count; ++i) {
            if (attrs->values[i].name.size == name.size &&
                memcmp(attrs->values[i].name.data, name.data, name.size) == 0)
                return sp5_failure(SP5_REASON_CONFLICT);
        }
        if (end > colon + 1 && sdp.data[end - 1] == ' ')
            --end;
        attrs->values[attrs->count++] = (attribute){name, {sdp.data + colon + 1, end - colon - 1}};
    }
    return sp5_success();
}
static sp5_result number(const attributes *attrs, const char *name, bool required, uint32_t *value)
{
    for (size_t i = 0; i < attrs->count; ++i) {
        if (!same(attrs->values[i].name, name))
            continue;
        sp5_bytes text = attrs->values[i].value;
        if (text.size == 0)
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        uint32_t result = 0;
        for (size_t j = 0; j < text.size; ++j) {
            uint8_t c = text.data[j];
            if (c < '0' || c > '9' || result > (UINT32_MAX - (c - '0')) / 10U)
                return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
            result = result * 10U + (c - '0');
        }
        *value = result;
        return sp5_success();
    }
    return required ? sp5_failure(SP5_REASON_INVALID_ARGUMENT) : sp5_success();
}
static sp5_result bitrate_value(const attributes *attrs, uint32_t minimum_kbps,
                                uint32_t maximum_kbps, sp5_bitrate *bitrate)
{
    if (minimum_kbps == 0 || maximum_kbps < minimum_kbps)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    uint32_t requested = 0;
    sp5_result result = number(attrs, "x-ml-video.configuredBitrateKbps", true, &requested);
    if (result.status != SP5_OK || requested == 0)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *bitrate = (sp5_bitrate){requested, requested, SP5_BITRATE_NONE};
    if (requested < minimum_kbps) {
        bitrate->chosen_kbps = minimum_kbps;
        bitrate->adjustment = SP5_BITRATE_RAISED;
    }
    if (requested > maximum_kbps) {
        bitrate->chosen_kbps = maximum_kbps;
        bitrate->adjustment = SP5_BITRATE_LOWERED;
    }
    return sp5_success();
}
sp5_result sp5_negotiate_bitrate(sp5_bytes sdp, uint32_t minimum_kbps, uint32_t maximum_kbps,
                                 sp5_bitrate *bitrate)
{
    if (bitrate == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *bitrate = (sp5_bitrate){0};
    attributes attrs;
    sp5_result result = parse(sdp, &attrs);
    return result.status == SP5_OK ? bitrate_value(&attrs, minimum_kbps, maximum_kbps, bitrate)
                                   : result;
}
#define GET(name, required, field)                                                                 \
    do {                                                                                           \
        sp5_result result = number(&attrs, name, required, &(field));                              \
        if (result.status != SP5_OK)                                                               \
            return result;                                                                         \
    } while (0)
sp5_result sp5_sdp_negotiate(sp5_bytes sdp, const sp5_host *host, sp5_negotiated *chosen)
{
    if (chosen == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *chosen = (sp5_negotiated){0};
    if (host == NULL || host->supported_mode_count > SP5_MODE_CAPACITY ||
        (host->encryption_supported != 0 &&
         ((host->encryption_supported & ~7U) != 0 || (host->encryption_supported & 1U) == 0)))
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    attributes attrs;
    sp5_result result = parse(sdp, &attrs);
    if (result.status != SP5_OK)
        return result;
    uint32_t width = 0, height = 0, fps = 0, codec = 0, range = 0, control = 0, encryption = 0,
             features = 0, packet = 0, channels = 0, mask = 0, duration = 5, nv_flags = 135,
             chroma = 0;
    /* Wire spellings/defaults: protocol-notes.md §4; mcc 41cedb8
     * SdpGenerator.c:177–536; Sunshine d0b4a88 rtsp.cpp:1075–1161. */
    GET("x-nv-video[0].clientViewportWd", true, width);
    GET("x-nv-video[0].clientViewportHt", true, height);
    GET("x-nv-video[0].maxFPS", true, fps);
    GET("x-nv-vqos[0].bitStreamFormat", false, codec);
    GET("x-nv-video[0].dynamicRangeMode", false, range);
    GET("x-nv-general.useReliableUdp", true, control);
    GET("x-ss-general.encryptionEnabled", false, encryption);
    GET("x-ml-general.featureFlags", false, features);
    GET("x-nv-video[0].packetSize", true, packet);
    GET("x-nv-audio.surround.numChannels", true, channels);
    GET("x-nv-audio.surround.channelMask", true, mask);
    GET("x-nv-aqos.packetDuration", false, duration);
    GET("x-nv-general.featureFlags", false, nv_flags);
    GET("x-ss-video[0].chromaSamplingType", false, chroma);
    if ((nv_flags & 32U) != 0)
        encryption |= 4U;
    /* Gen7 CONTROL_V2, optional VIDEO/AUDIO, stereo, 4:2:0. Flags are
     * client-enabled (mcc SdpGenerator.c:276-304 @ 41cedb8), with legacy
     * audio opt-in above. Packet ceiling is a local fixed-pool limit. */
    if (codec > 2 || range > 1 || control != 13 || (encryption & 1U) == 0 ||
        (encryption & ~(host->encryption_supported != 0 ? host->encryption_supported : 7U)) != 0 ||
        channels != 2 || mask != 3 || (duration != 5 && duration != 10) || packet < 64 ||
        packet > 2048 || chroma != 0)
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    const char *codecs[3] = {"h264", "hevc", "av1"};
    for (size_t i = 0; i < host->supported_mode_count; ++i) {
        const sp5_host_mode *m = &host->supported_modes[i];
        if (memchr(m->codec, 0, sizeof(m->codec)) == NULL || m->width != width ||
            m->height != height || m->fps != fps ||
            (strcmp(m->codec, codecs[codec]) != 0 &&
             !(codec == 1 && range == 1 && strcmp(m->codec, "hevc-main10") == 0)) ||
            m->dynamic_range != (range ? SP5_HDR_PQ_BT2020 : SP5_SDR))
            continue;
        sp5_negotiated next = {.mode_index = i,
                               .encryption_flags = encryption,
                               .feature_flags = features,
                               .packet_size = packet,
                               .audio_duration_ms = duration};
        result =
            bitrate_value(&attrs, m->minimum_bitrate_kbps, m->maximum_bitrate_kbps, &next.bitrate);
        if (result.status != SP5_OK)
            return result;
        *chosen = next;
        return sp5_success();
    }
    return sp5_failure(SP5_REASON_UNSUPPORTED);
}
#undef GET
static sp5_result text(sp5_buffer *out, const char *s)
{
    return sp5_buffer_append(out, (sp5_bytes){(const uint8_t *)s, strlen(s)});
}
sp5_result sp5_sdp_describe(const sp5_host *host, sp5_buffer *sdp)
{
    if (host == NULL || sdp == NULL || host->supported_mode_count > SP5_MODE_CAPACITY ||
        (host->encryption_supported != 0 &&
         ((host->encryption_supported & ~7U) != 0 || (host->encryption_supported & 1U) == 0)))
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    size_t original = sdp->size;
    bool hevc = false, av1 = false;
    for (size_t i = 0; i < host->supported_mode_count; ++i) {
        const sp5_host_mode *m = &host->supported_modes[i];
        if (memchr(m->codec, 0, sizeof(m->codec)) == NULL || m->width == 0 || m->height == 0 ||
            m->fps == 0 || m->minimum_bitrate_kbps == 0 ||
            m->maximum_bitrate_kbps < m->minimum_bitrate_kbps)
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        hevc |= strcmp(m->codec, "hevc") == 0 || strcmp(m->codec, "hevc-main10") == 0;
        av1 |= strcmp(m->codec, "av1") == 0;
    }
    /* §3 DESCRIBE source: Sunshine rtsp.cpp:878–943; mcc
     * RtspConnection.c:1090–1156. Support CONTROL_V2|VIDEO|AUDIO (1|2|4),
     * request only CONTROL_V2; optional media encryption remains client-selected.
     * No mode is inferred from a codec marker. */
    sp5_result result = text(sdp, "v=0\r\ns=SunshinePS5\r\n");
    if (result.status == SP5_OK)
        result = text(sdp, host->controller_touch_events ? "a=x-ss-general.featureFlags:2\r\n"
                                                         : "a=x-ss-general.featureFlags:0\r\n");
    if (result.status == SP5_OK) {
        char encryption[128];
        int size = snprintf(
            encryption, sizeof(encryption),
            "a=x-ss-general.encryptionSupported:%u\r\na=x-ss-general.encryptionRequested:1\r\n",
            host->encryption_supported != 0 ? host->encryption_supported : 7U);
        if (size < 0 || (size_t)size >= sizeof(encryption))
            result = sp5_failure(SP5_REASON_CAPACITY);
        else
            result = text(sdp, encryption);
    }
    if (result.status == SP5_OK && hevc)
        result = text(sdp, "sprop-parameter-sets=AAAAAU\r\n");
    if (result.status == SP5_OK && av1)
        result = text(sdp, "a=rtpmap:98 AV1/90000\r\n");
    if (result.status != SP5_OK)
        sdp->size = original;
    return result;
}
