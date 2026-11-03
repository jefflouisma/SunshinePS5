#include "encode/ps5.h"

#include "capture/ps5.h"
#include "firmware/13.60.h"
#include "ps5_annexb.h"
#include "ps5_clock.h"
#include "ps5_media.h"
#include "runtime.h"

#include <string.h>

// NOLINTBEGIN(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
static sp5_result encoder_open(void *context, const sp5_media_config *config)
{
    sp5_ps5_encoder *encoder = context;
    if (encoder == NULL || config == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (encoder->opened)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    sp5_ps5_mode mode;
    if (!sp5_ps5_mode_validate(config, &mode))
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    if (mode.ceiling_kbps != 0 && config->chosen_bitrate_kbps > mode.ceiling_kbps)
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    if (!sp5_ps5_firmware_supported()) {
        encoder->failure = SP5_REASON_UNSUPPORTED;
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    }
    *encoder = (sp5_ps5_encoder){.opened = true,
                                 .width = config->width,
                                 .height = config->height,
                                 .fps = mode.fps,
                                 .codec = mode.codec,
                                 .depth = mode.depth,
                                 .chosen_bitrate_kbps = config->chosen_bitrate_kbps,
                                 .dynamic_range = config->dynamic_range,
                                 .encoded_frames = 0,
                                 .failure = SP5_REASON_NONE};
    return sp5_success();
}

static sp5_result encoder_encode(void *context, const sp5_video_frame *frame, bool force_key_frame,
                                 sp5_timestamp deadline, bool metrics, sp5_buffer *output,
                                 sp5_encoded_frame *encoded)
{
    sp5_ps5_encoder *encoder = context;
    if (encoder == NULL || frame == NULL || output == NULL || encoded == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *encoded = (sp5_encoded_frame){0};
    output->size = 0;
    if (!encoder->opened)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    if (output->data == NULL || output->capacity == 0)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    const char *format = sp5_ps5_format_ref(encoder->codec, encoder->depth);
    if (format == NULL || frame->format_ref == NULL || strcmp(frame->format_ref, format) != 0 ||
        frame->surface == NULL)
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    const sp5_ps5_capture_surface *surface = frame->surface;
    if (surface->magic != SP5_PS5_CAPTURE_SURFACE_MAGIC ||
        surface->version != SP5_PS5_CAPTURE_SURFACE_VERSION || surface->data == NULL ||
        surface->size == 0 || surface->size > SP5_PS5_CAPTURE_READ_BYTES ||
        surface->codec != encoder->codec || surface->depth != encoder->depth ||
        surface->width != encoder->width || surface->height != encoder->height ||
        surface->frame_id != frame->frame_id) {
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    }
    sp5_ps5_annexb_class classification =
        sp5_ps5_annexb_classify(surface->data, surface->size, surface->codec);
    if (classification == SP5_PS5_ANNEXB_INVALID)
        return sp5_failure(SP5_REASON_PLATFORM);
    bool key_frame = classification == SP5_PS5_ANNEXB_KEY;
    /* No direct Venc request-keyframe call was recovered. Refuse to pretend a
     * forced IDR happened; otherwise the native access-unit type is reported. */
    if (force_key_frame && !key_frame)
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    if (surface->size > output->capacity)
        return sp5_failure(SP5_REASON_CAPACITY);
    if (encoder->encoded_frames == UINT64_MAX)
        return sp5_failure(SP5_REASON_OVERFLOW);
    sp5_result result = sp5_ps5_clock_check_deadline(deadline);
    if (result.status != SP5_OK)
        return result;
    sp5_timestamp start = {0};
    uint64_t start_us = 0;
    if (metrics) {
        result = sp5_ps5_clock_now(&start, &start_us);
        if (result.status != SP5_OK)
            return result;
        (void)start_us;
    }
    memcpy(output->data, surface->data, surface->size);
    sp5_timestamp end = {0};
    uint64_t end_us = 0;
    result = sp5_ps5_clock_now(&end, &end_us);
    if (result.status != SP5_OK)
        return result;
    (void)end_us;
    if (end.ticks > deadline.ticks)
        return sp5_failure(SP5_REASON_TIMEOUT);
    output->size = surface->size;
    /* Provisional handoff span: bitstream validation plus bounded copy. It does
     * not measure Venc internally; that completion happened inside Avcap2. */
    *encoded = (sp5_encoded_frame){
        .frame_id = frame->frame_id,
        .source_epoch = frame->source_epoch,
        .key_frame = key_frame,
        .encode_span = metrics ? (sp5_stage_span){start, end, SP5_VALIDITY_VALID, SP5_REASON_NONE}
                               : (sp5_stage_span){.reason = SP5_REASON_METRICS_DISABLED}};
    ++encoder->encoded_frames;
    encoder->failure = SP5_REASON_NONE;
    return sp5_success();
}

static sp5_result encoder_close(void *context)
{
    sp5_ps5_encoder *encoder = context;
    if (encoder == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    /* No platform resource was acquired by open; idempotent. */
    encoder->opened = false;
    encoder->failure = SP5_REASON_NONE;
    return sp5_success();
}

void sp5_ps5_encoder_init(sp5_ps5_encoder *encoder)
{
    if (encoder == NULL)
        return;
    memset(encoder, 0, sizeof(*encoder));
    encoder->failure = SP5_REASON_NONE;
}

sp5_encode_ops sp5_ps5_encoder_operations(sp5_ps5_encoder *encoder)
{
    return (sp5_encode_ops){
        .context = encoder, .open = encoder_open, .encode = encoder_encode, .close = encoder_close};
}

void sp5_ps5_encoder_provenance(const sp5_ps5_encoder *encoder, sp5_platform_provenance *provenance)
{
    (void)encoder;
    if (provenance == NULL)
        return;
    *provenance = (sp5_platform_provenance){
        .synthetic = false,
        .implementation_ref =
            "platform/encode/ps5.c (provisional; Avcap2/Venc bitstream forwarding, not a "
            "direct Venc client)",
        .firmware = "13.60 (13.600.007)",
        .capability_evidence_ref = "specs/001-moonlight-streaming-host/research-r3-findings.md"
                                   "#t041-required-evidence-ledger",
        .validity = SP5_VALIDITY_INVALID,
        .reason = SP5_REASON_UNVERIFIED};
}

// NOLINTEND(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
