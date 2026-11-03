#include "platform.h"

static sp5_result called(sp5_fake_platform *fake, sp5_fake_operation operation)
{
    ++fake->calls[operation];
    ++fake->sequence;
    sp5_reason reason = fake->fail_next[operation];
    fake->fail_next[operation] = SP5_REASON_NONE;
    return reason == SP5_REASON_NONE ? sp5_success() : sp5_failure(reason);
}

static sp5_result protection(const sp5_fake_platform *fake)
{
    if (fake->source.protection != SP5_PROTECTION_CLEAR) {
        return sp5_failure(fake->source.protection == SP5_PROTECTION_ACTIVE
                               ? SP5_REASON_PROTECTED
                               : SP5_REASON_PROTECTION_UNKNOWN);
    }
    return sp5_success();
}

static sp5_result check_deadline(const sp5_fake_platform *fake, sp5_timestamp deadline)
{
    if (deadline.validity != SP5_VALIDITY_VALID || fake->now.validity != SP5_VALIDITY_VALID ||
        fake->clock_description.validity != SP5_VALIDITY_VALID ||
        !fake->clock_description.monotonic || fake->clock_description.domain != fake->now.domain ||
        fake->clock_description.epoch != fake->now.epoch || deadline.domain != fake->now.domain ||
        deadline.epoch != fake->now.epoch) {
        return sp5_failure(SP5_REASON_CLOCK);
    }
    return deadline.ticks < fake->now.ticks ? sp5_failure(SP5_REASON_TIMEOUT) : sp5_success();
}

static bool config_valid(const sp5_media_config *config)
{
    return config != NULL && config->width != 0 && config->height != 0 &&
           config->fps_numerator != 0 && config->fps_denominator != 0 && config->codec != NULL &&
           config->chosen_bitrate_kbps != 0 &&
           (config->dynamic_range == SP5_SDR || config->dynamic_range == SP5_HDR_PQ_BT2020);
}

static sp5_stage_span span(const sp5_fake_platform *fake, bool metrics)
{
    if (!metrics) {
        return (sp5_stage_span){.reason = SP5_REASON_METRICS_DISABLED};
    }
    /* Equal endpoints are intentional deterministic fixture observations. */
    return (sp5_stage_span){fake->now, fake->now, SP5_VALIDITY_VALID, SP5_REASON_NONE};
}

static sp5_result capture_open(void *context, const sp5_media_config *config)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_CAPTURE_OPEN);
    if (result.status != SP5_OK) {
        return result;
    }
    if (!config_valid(config)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (fake->capture_open) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    uint64_t previous_epoch = fake->source.epoch;
    bool resized = fake->source.width != config->width || fake->source.height != config->height;
    if (resized && previous_epoch == UINT64_MAX) {
        return sp5_failure(SP5_REASON_OVERFLOW);
    }
    result = sp5_fake_set_source(fake, fake->source.protection, config->dynamic_range);
    if (result.status != SP5_OK) {
        return result;
    }
    if (resized && fake->source.epoch == previous_epoch) {
        ++fake->source.epoch;
    }
    fake->source.width = config->width;
    fake->source.height = config->height;
    fake->capture_open = true;
    return sp5_success();
}

static sp5_result source_state(void *context, sp5_source_state *state)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_SOURCE_STATE);
    if (state == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *state = (sp5_source_state){0};
    if (result.status == SP5_OK) {
        *state = fake->source;
        state->observed_at = fake->now;
    }
    return result;
}

static sp5_result capture_acquire(void *context, sp5_timestamp deadline, bool metrics,
                                  sp5_video_frame *frame)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_CAPTURE_ACQUIRE);
    if (frame == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *frame = (sp5_video_frame){0};
    if (result.status != SP5_OK) {
        return result;
    }
    if (!fake->capture_open) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    result = protection(fake);
    if (result.status == SP5_OK) {
        result = check_deadline(fake, deadline);
    }
    if (result.status != SP5_OK) {
        return result;
    }
    if (fake->video_leased) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    if (fake->next_lease == UINT64_MAX || fake->frame_id == UINT64_MAX) {
        return sp5_failure(SP5_REASON_OVERFLOW);
    }
    for (size_t i = 0; i < 8; ++i) {
        fake->video_samples[i] = (uint16_t)(i * (fake->source.dynamic_range == SP5_SDR ? 32 : 128));
    }
    fake->video_lease = ++fake->next_lease;
    fake->video_leased = true;
    *frame = (sp5_video_frame){.lease_id = fake->video_lease,
                               .frame_id = ++fake->frame_id,
                               .source_epoch = fake->source.epoch,
                               .surface = fake->video_samples,
                               .format_ref = "synthetic/tile-samples-v1 (not a pixel plane)",
                               .capture_timestamp = fake->now,
                               .capture_span = span(fake, metrics)};
    return sp5_success();
}

static sp5_result capture_release(void *context, uint64_t lease_id)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_CAPTURE_RELEASE);
    if (result.status != SP5_OK) {
        return result;
    }
    if (!fake->video_leased || lease_id != fake->video_lease) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    fake->video_leased = false;
    return sp5_success();
}

static sp5_result capture_close(void *context)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_CAPTURE_CLOSE);
    if (result.status != SP5_OK) {
        return result;
    }
    if (fake->video_leased) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    if (fake->capture_open) {
        fake->capture_open = false;
        ++fake->capture_closes;
        fake->last_media_close_sequence = fake->sequence;
    }
    return sp5_success();
}

static sp5_result encoder_open(void *context, const sp5_media_config *config)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_ENCODER_OPEN);
    if (result.status != SP5_OK) {
        return result;
    }
    if (!config_valid(config)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (fake->encoder_open) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    fake->encoder_open = true;
    return sp5_success();
}

static sp5_result encode(void *context, const sp5_video_frame *frame, bool force_key_frame,
                         sp5_timestamp deadline, bool metrics, sp5_buffer *output,
                         sp5_encoded_frame *encoded)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_ENCODE);
    if (encoded != NULL) {
        *encoded = (sp5_encoded_frame){0};
    }
    if (output != NULL) {
        output->size = 0;
    }
    if (frame == NULL || output == NULL || encoded == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (result.status != SP5_OK) {
        return result;
    }
    if (!fake->encoder_open || !fake->video_leased || frame->lease_id != fake->video_lease ||
        frame->surface != fake->video_samples || frame->frame_id != fake->frame_id) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    if (frame->source_epoch != fake->source.epoch) {
        return sp5_failure(SP5_REASON_SOURCE_CHANGED);
    }
    result = protection(fake);
    if (result.status == SP5_OK) {
        result = check_deadline(fake, deadline);
    }
    if (result.status != SP5_OK) {
        return result;
    }
    /* Deliberately not a playable H.264/HEVC bitstream or hardware evidence. */
    const uint8_t payload[] = {'S', 'Y', 'N', 'T', 'H', (uint8_t)fake->source.dynamic_range};
    result = sp5_buffer_append(output, (sp5_bytes){payload, sizeof(payload)});
    if (result.status == SP5_OK) {
        *encoded = (sp5_encoded_frame){frame->frame_id, frame->source_epoch, force_key_frame,
                                       span(fake, metrics)};
    }
    return result;
}

static sp5_result encoder_close(void *context)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_ENCODER_CLOSE);
    if (result.status == SP5_OK && fake->encoder_open) {
        fake->encoder_open = false;
        ++fake->encoder_closes;
        fake->last_media_close_sequence = fake->sequence;
    }
    return result;
}

static sp5_result audio_open(void *context, uint32_t sample_rate)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_AUDIO_OPEN);
    if (result.status != SP5_OK) {
        return result;
    }
    if (sample_rate == 0) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (fake->audio_open) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    fake->sample_rate = sample_rate;
    fake->audio_open = true;
    return sp5_success();
}

static sp5_result audio_acquire(void *context, sp5_timestamp deadline, sp5_audio_block *block)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_AUDIO_ACQUIRE);
    if (block == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *block = (sp5_audio_block){0};
    if (result.status != SP5_OK) {
        return result;
    }
    if (!fake->audio_open) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    result = protection(fake);
    if (result.status == SP5_OK) {
        result = check_deadline(fake, deadline);
    }
    if (result.status != SP5_OK) {
        return result;
    }
    if (fake->audio_leased) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    if (fake->next_lease == UINT64_MAX) {
        return sp5_failure(SP5_REASON_OVERFLOW);
    }
    fake->audio_lease = ++fake->next_lease;
    fake->audio_leased = true;
    *block = (sp5_audio_block){
        .lease_id = fake->audio_lease,
        .source_epoch = fake->source.epoch,
        .timestamp = fake->now,
        .samples = {(const uint8_t *)fake->audio_samples, sizeof(fake->audio_samples)},
        .sample_rate = fake->sample_rate,
        .frames = 4,
        .channels = 2,
        .sample_format_ref = "synthetic/stereo-s16-native"};
    return sp5_success();
}

static sp5_result audio_release(void *context, uint64_t lease_id)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_AUDIO_RELEASE);
    if (result.status != SP5_OK) {
        return result;
    }
    if (!fake->audio_leased || lease_id != fake->audio_lease) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    fake->audio_leased = false;
    return sp5_success();
}

static sp5_result audio_close(void *context)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_AUDIO_CLOSE);
    if (result.status != SP5_OK) {
        return result;
    }
    if (fake->audio_leased) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    if (fake->audio_open) {
        fake->audio_open = false;
        ++fake->audio_closes;
        fake->last_media_close_sequence = fake->sequence;
    }
    return sp5_success();
}

static sp5_result input_add(void *context, uint64_t session_id, uint32_t controller_id)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_INPUT_ADD);
    if (result.status != SP5_OK) {
        return result;
    }
    if (controller_id >= SP5_MAX_CONTROLLERS) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    if (fake->controllers[controller_id].live) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    fake->controllers[controller_id] =
        (sp5_fake_controller){.live = true, .session_id = session_id};
    return sp5_success();
}

static sp5_result input_deliver(void *context, const sp5_controller_state *state, bool metrics,
                                sp5_input_evidence *evidence)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_INPUT_DELIVER);
    if (evidence == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    evidence->console_delivered = (sp5_timestamp){0};
    evidence->delivery_boundary_ref = NULL;
    evidence->delivery = SP5_INPUT_DELIVERY_FAILED;
    evidence->reason = result.reason;
    if (result.status != SP5_OK) {
        return result;
    }
    if (state == NULL || evidence->controller_id >= SP5_MAX_CONTROLLERS) {
        evidence->reason = SP5_REASON_INVALID_ARGUMENT;
        return sp5_failure(evidence->reason);
    }
    sp5_fake_controller *controller = &fake->controllers[evidence->controller_id];
    if (!controller->live || controller->session_id != evidence->session_id) {
        evidence->reason = SP5_REASON_OWNERSHIP;
        return sp5_failure(evidence->reason);
    }
    controller->state = *state;
    evidence->delivery =
        fake->confirm_delivery ? SP5_INPUT_DELIVERY_CONFIRMED : SP5_INPUT_DELIVERY_UNCONFIRMED;
    evidence->reason = fake->confirm_delivery ? SP5_REASON_NONE : SP5_REASON_DELIVERY_UNCONFIRMED;
    if (fake->confirm_delivery) {
        evidence->delivery_boundary_ref = "synthetic/controller-state-copy (not console delivery)";
        if (metrics) {
            evidence->console_delivered = fake->now;
        }
    }
    return sp5_success();
}

static sp5_result input_feedback(void *context, sp5_controller_feedback *feedback)
{
    sp5_result result = called(context, SP5_FAKE_INPUT_FEEDBACK);
    if (feedback == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *feedback = (sp5_controller_feedback){0};
    return result.status == SP5_OK ? sp5_failure(SP5_REASON_WOULD_BLOCK) : result;
}

static sp5_result input_remove(void *context, uint64_t session_id, uint32_t controller_id)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_INPUT_REMOVE);
    if (result.status != SP5_OK) {
        return result;
    }
    if (controller_id >= SP5_MAX_CONTROLLERS ||
        (fake->controllers[controller_id].live &&
         fake->controllers[controller_id].session_id != session_id)) {
        return sp5_failure(controller_id >= SP5_MAX_CONTROLLERS ? SP5_REASON_INVALID_ARGUMENT
                                                                : SP5_REASON_OWNERSHIP);
    }
    fake->controllers[controller_id] = (sp5_fake_controller){0};
    return sp5_success();
}

static sp5_result input_release_all(void *context, uint64_t session_id)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_INPUT_RELEASE_ALL);
    if (result.status != SP5_OK) {
        return result;
    }
    for (size_t i = 0; i < SP5_MAX_CONTROLLERS; ++i) {
        if (fake->controllers[i].live && fake->controllers[i].session_id == session_id) {
            fake->controllers[i] = (sp5_fake_controller){0};
        }
    }
    fake->last_input_release_sequence = fake->sequence;
    return sp5_success();
}

static sp5_result system_probe(void *context, sp5_platform_provenance *provenance)
{
    sp5_result result = called(context, SP5_FAKE_SYSTEM_PROBE);
    if (provenance == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *provenance = (sp5_platform_provenance){
        .synthetic = true,
        .implementation_ref = "tests/fakes/platform.c",
        .firmware = "synthetic/no-console",
        .capability_evidence_ref = "synthetic/unit-tests-only",
        .validity = result.status == SP5_OK ? SP5_VALIDITY_VALID : SP5_VALIDITY_INVALID,
        .reason = result.reason};
    return result;
}

static sp5_result system_prepare(void *context)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_SYSTEM_PREPARE);
    if (result.status != SP5_OK) {
        return result;
    }
    if (fake->system_state.validity != SP5_VALIDITY_VALID) {
        return sp5_failure(SP5_REASON_UNVERIFIED);
    }
    if (fake->system_state.remote_play || fake->system_state.share_play ||
        fake->system_state.stopping_or_rest) {
        return sp5_failure(SP5_REASON_CONFLICT);
    }
    if (fake->prepared) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    fake->prepared = true;
    return sp5_success();
}

static sp5_result system_state(void *context, sp5_system_state *state)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_SYSTEM_STATE);
    if (state == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *state = result.status == SP5_OK
                 ? fake->system_state
                 : (sp5_system_state){.validity = SP5_VALIDITY_INVALID, .reason = result.reason};
    return result;
}

static sp5_result system_notify(void *context, const char *message, size_t size)
{
    sp5_result result = called(context, SP5_FAKE_SYSTEM_NOTIFY);
    if (message == NULL && size != 0) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    return size > 256 ? sp5_failure(SP5_REASON_CAPACITY) : result;
}

static sp5_result system_rollback(void *context)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_SYSTEM_ROLLBACK);
    if (result.status == SP5_OK && fake->prepared) {
        fake->prepared = false;
        ++fake->rollbacks;
    }
    return result;
}

static sp5_result clock_describe(void *context, sp5_clock_description *description)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_CLOCK_DESCRIBE);
    if (description == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *description =
        result.status == SP5_OK
            ? fake->clock_description
            : (sp5_clock_description){.validity = SP5_VALIDITY_INVALID, .reason = result.reason};
    return result;
}

static sp5_result clock_now(void *context, sp5_timestamp *timestamp)
{
    sp5_fake_platform *fake = context;
    sp5_result result = called(fake, SP5_FAKE_CLOCK_NOW);
    if (timestamp == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *timestamp = result.status == SP5_OK
                     ? fake->now
                     : (sp5_timestamp){.validity = SP5_VALIDITY_INVALID, .reason = result.reason};
    if (result.status == SP5_OK && timestamp->validity != SP5_VALIDITY_VALID) {
        return sp5_failure(timestamp->reason == SP5_REASON_NONE ? SP5_REASON_CLOCK
                                                                : timestamp->reason);
    }
    return result;
}

sp5_result sp5_fake_advance(sp5_fake_platform *fake, uint64_t ticks)
{
    if (fake == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (ticks > UINT64_MAX - fake->now.ticks) {
        return sp5_failure(SP5_REASON_OVERFLOW);
    }
    fake->now.ticks += ticks;
    return sp5_success();
}

sp5_result sp5_fake_set_source(sp5_fake_platform *fake, sp5_protection protected_state,
                               sp5_dynamic_range range)
{
    if (fake == NULL ||
        (protected_state != SP5_PROTECTION_UNKNOWN && protected_state != SP5_PROTECTION_CLEAR &&
         protected_state != SP5_PROTECTION_ACTIVE) ||
        (range != SP5_SDR && range != SP5_HDR_PQ_BT2020)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (fake->source.protection != protected_state || fake->source.dynamic_range != range) {
        if (fake->source.epoch == UINT64_MAX) {
            return sp5_failure(SP5_REASON_OVERFLOW);
        }
        ++fake->source.epoch;
    }
    fake->source.protection = protected_state;
    fake->source.dynamic_range = range;
    fake->source.bit_depth = range == SP5_SDR ? 8 : 10;
    fake->source.observed_at = fake->now;
    fake->source.hdr =
        range == SP5_SDR
            ? (sp5_hdr_metadata){.reason = SP5_REASON_STAGE_UNAVAILABLE}
            : (sp5_hdr_metadata){.validity = SP5_VALIDITY_VALID,
                                 .format_ref = "synthetic/hdr-fixture-v1",
                                 .source_evidence_ref = "synthetic/not-console-metadata",
                                 .payload = {fake->hdr_metadata, sizeof(fake->hdr_metadata)}};
    return sp5_success();
}

void sp5_fake_init(sp5_fake_platform *fake)
{
    *fake = (sp5_fake_platform){0};
    fake->capture = (sp5_capture_ops){
        fake, capture_open, source_state, capture_acquire, capture_release, capture_close};
    fake->encoder = (sp5_encode_ops){fake, encoder_open, encode, encoder_close};
    fake->audio = (sp5_audio_ops){fake, audio_open, audio_acquire, audio_release, audio_close};
    fake->input = (sp5_input_ops){fake,           input_add,    input_deliver,
                                  input_feedback, input_remove, input_release_all};
    fake->system = (sp5_system_ops){fake,         system_probe,  system_prepare,
                                    system_state, system_notify, system_rollback};
    fake->clock = (sp5_clock_ops){fake, clock_describe, clock_now};
    fake->now = (sp5_timestamp){.domain = 1, .epoch = 1, .validity = SP5_VALIDITY_VALID};
    fake->clock_description = (sp5_clock_description){.domain = 1,
                                                      .epoch = 1,
                                                      .ticks_per_second = 1000000,
                                                      .native_resolution_ticks = 1,
                                                      .monotonic = true,
                                                      .verified_uncertainty_ns = {true, 0},
                                                      .validity = SP5_VALIDITY_VALID};
    fake->source = (sp5_source_state){.epoch = 1,
                                      .observed_at = fake->now,
                                      .protection = SP5_PROTECTION_CLEAR,
                                      .dynamic_range = SP5_SDR,
                                      .width = 2,
                                      .height = 2,
                                      .bit_depth = 8,
                                      .hdr = {.reason = SP5_REASON_STAGE_UNAVAILABLE}};
    fake->system_state.validity = SP5_VALIDITY_VALID;
    fake->confirm_delivery = true;
    for (size_t i = 0; i < 8; ++i) {
        fake->video_samples[i] = (uint16_t)(i * 32);
        fake->audio_samples[i] = i % 2 == 0 ? 100 : -100;
    }
    fake->hdr_metadata[0] = 10;
    fake->hdr_metadata[1] = 20;
    fake->hdr_metadata[2] = 30;
    fake->hdr_metadata[3] = 40;
}
