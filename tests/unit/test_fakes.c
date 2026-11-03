#include "../fakes/platform.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            if (fputs(#condition "\n", stderr) == EOF) {                                           \
                return 2;                                                                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static sp5_media_config config(void)
{
    return (sp5_media_config){2, 2, 60, 1, "hevc", SP5_SDR, 10000};
}

static sp5_timestamp deadline(sp5_fake_platform *fake)
{
    sp5_timestamp stamp = fake->now;
    stamp.ticks += 1000000;
    return stamp;
}

static int media(void)
{
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_media_config mode = config();
    sp5_platform_provenance provenance;
    CHECK(fake.system.probe(fake.system.context, &provenance).status == SP5_OK);
    CHECK(provenance.synthetic && strstr(provenance.firmware, "synthetic") != NULL);
    fake.fail_next[SP5_FAKE_CAPTURE_OPEN] = SP5_REASON_PLATFORM;
    CHECK(fake.capture.open(&fake, &mode).reason == SP5_REASON_PLATFORM);
    CHECK(!fake.capture_open);
    CHECK(fake.capture.open(&fake, &mode).status == SP5_OK);
    CHECK(fake.capture.open(&fake, &mode).reason == SP5_REASON_OWNERSHIP);
    CHECK(fake.encoder.open(&fake, &mode).status == SP5_OK);
    CHECK(fake.audio.open(&fake, 48000).status == SP5_OK);
    sp5_video_frame frame;
    CHECK(fake.capture.acquire(&fake, deadline(&fake), false, &frame).status == SP5_OK);
    CHECK(frame.capture_timestamp.validity == SP5_VALIDITY_VALID);
    CHECK(frame.capture_span.validity == SP5_VALIDITY_UNAVAILABLE);
    CHECK(frame.surface != NULL && frame.source_epoch == fake.source.epoch);
    uint64_t lease = frame.lease_id;
    sp5_video_frame ignored;
    CHECK(fake.capture.acquire(&fake, deadline(&fake), true, &ignored).reason ==
          SP5_REASON_WOULD_BLOCK);
    CHECK(fake.capture.release(&fake, lease + 1).reason == SP5_REASON_OWNERSHIP);
    uint8_t bytes[32];
    sp5_buffer output = {bytes, sizeof(bytes), 0};
    sp5_encoded_frame encoded;
    CHECK(
        fake.encoder.encode(&fake, &frame, true, deadline(&fake), true, &output, &encoded).status ==
        SP5_OK);
    CHECK(output.size > 0 && encoded.key_frame &&
          encoded.encode_span.validity == SP5_VALIDITY_VALID);
    CHECK(memcmp(bytes, "SYNTH", 5) == 0);
    output.capacity = 1;
    CHECK(fake.encoder.encode(&fake, &frame, false, deadline(&fake), false, &output, &encoded)
              .reason == SP5_REASON_CAPACITY);
    CHECK(output.size == 0 && fake.video_leased);
    output.capacity = sizeof(bytes);
    fake.fail_next[SP5_FAKE_ENCODE] = SP5_REASON_PLATFORM;
    CHECK(fake.encoder.encode(&fake, &frame, false, deadline(&fake), false, &output, &encoded)
              .reason == SP5_REASON_PLATFORM);
    CHECK(fake.video_leased && output.size == 0);
    CHECK(sp5_fake_set_source(&fake, SP5_PROTECTION_ACTIVE, SP5_HDR_PQ_BT2020).status == SP5_OK);
    CHECK(fake.encoder.encode(&fake, &frame, false, deadline(&fake), false, &output, &encoded)
              .reason == SP5_REASON_SOURCE_CHANGED);
    CHECK(fake.capture.release(&fake, lease).status == SP5_OK);
    CHECK(fake.capture.release(&fake, lease).reason == SP5_REASON_OWNERSHIP);
    CHECK(fake.capture.acquire(&fake, deadline(&fake), true, &frame).reason ==
          SP5_REASON_PROTECTED);
    sp5_audio_block audio;
    CHECK(fake.audio.acquire(&fake, deadline(&fake), &audio).reason == SP5_REASON_PROTECTED);
    CHECK(sp5_fake_set_source(&fake, SP5_PROTECTION_UNKNOWN, SP5_SDR).status == SP5_OK);
    CHECK(fake.capture.acquire(&fake, deadline(&fake), true, &frame).reason ==
          SP5_REASON_PROTECTION_UNKNOWN);
    CHECK(sp5_fake_set_source(&fake, SP5_PROTECTION_CLEAR, SP5_HDR_PQ_BT2020).status == SP5_OK);
    sp5_source_state source;
    CHECK(fake.capture.source_state(&fake, &source).status == SP5_OK);
    CHECK(source.bit_depth == 10 && source.hdr.validity == SP5_VALIDITY_VALID);
    CHECK(strstr(source.hdr.source_evidence_ref, "synthetic") != NULL);
    CHECK(fake.capture.acquire(&fake, deadline(&fake), true, &frame).status == SP5_OK);
    CHECK(frame.lease_id != lease && frame.capture_span.validity == SP5_VALIDITY_VALID);
    CHECK(fake.capture.close(&fake).reason == SP5_REASON_OWNERSHIP);
    CHECK(fake.capture.release(&fake, frame.lease_id).status == SP5_OK);
    CHECK(fake.audio.acquire(&fake, deadline(&fake), &audio).status == SP5_OK);
    CHECK(audio.channels == 2 && audio.sample_rate == 48000 && audio.samples.size > 0);
    CHECK(audio.source_epoch == source.epoch);
    CHECK(fake.audio.close(&fake).reason == SP5_REASON_OWNERSHIP);
    CHECK(fake.audio.release(&fake, audio.lease_id).status == SP5_OK);
    CHECK(fake.audio.release(&fake, audio.lease_id).reason == SP5_REASON_OWNERSHIP);
    CHECK(fake.audio.close(&fake).status == SP5_OK);
    CHECK(fake.encoder.close(&fake).status == SP5_OK);
    CHECK(fake.capture.close(&fake).status == SP5_OK);
    CHECK(fake.capture.close(&fake).status == SP5_OK);
    CHECK(fake.capture_closes == 1 && fake.encoder_closes == 1 && fake.audio_closes == 1);
    return 0;
}

static int independent_input(void)
{
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_media_config mode = config();
    CHECK(fake.system.prepare(&fake).status == SP5_OK);
    CHECK(fake.capture.open(&fake, &mode).status == SP5_OK);
    sp5_video_frame frame;
    CHECK(fake.capture.acquire(&fake, deadline(&fake), true, &frame).status == SP5_OK);
    CHECK(fake.input.add(&fake, 8, 0).status == SP5_OK);
    CHECK(fake.input.add(&fake, 9, 1).status == SP5_OK);
    CHECK(fake.input.add(&fake, 8, SP5_MAX_CONTROLLERS).reason == SP5_REASON_CAPACITY);
    sp5_controller_state state = {.buttons = 7, .axes = {12, 34}, .triggers = {1, 2}};
    sp5_input_evidence event = {
        .session_id = 8, .event_id = 10, .controller_id = 0, .host_received = fake.now};
    uint64_t clock_reads = fake.calls[SP5_FAKE_CLOCK_NOW];
    CHECK(fake.input.deliver(&fake, &state, false, &event).status == SP5_OK);
    CHECK(event.delivery == SP5_INPUT_DELIVERY_CONFIRMED);
    CHECK(event.console_delivered.validity == SP5_VALIDITY_UNAVAILABLE);
    CHECK(fake.calls[SP5_FAKE_CLOCK_NOW] == clock_reads && fake.video_leased);
    CHECK(fake.controllers[0].state.buttons == 7);
    CHECK(fake.input.deliver(&fake, &state, true, &event).status == SP5_OK);
    CHECK(event.console_delivered.validity == SP5_VALIDITY_VALID);
    CHECK(strstr(event.delivery_boundary_ref, "synthetic") != NULL);
    fake.confirm_delivery = false;
    CHECK(fake.input.deliver(&fake, &state, true, &event).status == SP5_OK);
    CHECK(event.delivery == SP5_INPUT_DELIVERY_UNCONFIRMED);
    CHECK(event.console_delivered.validity == SP5_VALIDITY_UNAVAILABLE);
    fake.fail_next[SP5_FAKE_INPUT_DELIVER] = SP5_REASON_PLATFORM;
    CHECK(fake.input.deliver(&fake, &state, true, &event).reason == SP5_REASON_PLATFORM);
    CHECK(event.delivery == SP5_INPUT_DELIVERY_FAILED);
    CHECK(event.console_delivered.validity == SP5_VALIDITY_UNAVAILABLE);
    CHECK(fake.input.remove(&fake, 9, 0).reason == SP5_REASON_OWNERSHIP);
    fake.fail_next[SP5_FAKE_INPUT_RELEASE_ALL] = SP5_REASON_PLATFORM;
    CHECK(fake.input.release_all(&fake, 8).reason == SP5_REASON_PLATFORM);
    CHECK(fake.controllers[0].live);
    CHECK(fake.input.release_all(&fake, 8).status == SP5_OK);
    CHECK(!fake.controllers[0].live && fake.controllers[0].state.buttons == 0);
    CHECK(fake.controllers[1].live);
    CHECK(fake.input.release_all(&fake, 8).status == SP5_OK);
    CHECK(fake.capture.release(&fake, frame.lease_id).status == SP5_OK);
    CHECK(fake.capture.close(&fake).status == SP5_OK);
    CHECK(fake.last_input_release_sequence < fake.last_media_close_sequence);
    CHECK(fake.input.release_all(&fake, 9).status == SP5_OK);
    CHECK(fake.system.rollback(&fake).status == SP5_OK);
    CHECK(fake.system.rollback(&fake).status == SP5_OK && fake.rollbacks == 1);
    CHECK(!fake.prepared && !fake.controllers[1].live);
    return 0;
}

static int clocks_and_faults(void)
{
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_clock_description description;
    sp5_timestamp now;
    CHECK(fake.clock.describe(&fake, &description).status == SP5_OK);
    CHECK(description.monotonic && description.ticks_per_second == 1000000);
    CHECK(sp5_fake_advance(&fake, 500).status == SP5_OK);
    CHECK(fake.clock.now(&fake, &now).status == SP5_OK && now.ticks == 500);
    fake.fail_next[SP5_FAKE_CLOCK_NOW] = SP5_REASON_CLOCK;
    CHECK(fake.clock.now(&fake, &now).reason == SP5_REASON_CLOCK);
    CHECK(now.validity == SP5_VALIDITY_INVALID);
    CHECK(fake.clock.now(&fake, &now).status == SP5_OK);
    fake.now.ticks = UINT64_MAX;
    CHECK(sp5_fake_advance(&fake, 1).reason == SP5_REASON_OVERFLOW);
    fake.system_state.remote_play = true;
    CHECK(fake.system.prepare(&fake).reason == SP5_REASON_CONFLICT && !fake.prepared);
    fake.system_state.remote_play = false;
    fake.fail_next[SP5_FAKE_SYSTEM_PREPARE] = SP5_REASON_PLATFORM;
    CHECK(fake.system.prepare(&fake).reason == SP5_REASON_PLATFORM && !fake.prepared);
    sp5_system_state system_state;
    CHECK(fake.system.state(&fake, &system_state).status == SP5_OK);
    CHECK(!system_state.remote_play);
    sp5_controller_feedback feedback;
    CHECK(fake.input.poll_feedback(&fake, &feedback).reason == SP5_REASON_WOULD_BLOCK);
    CHECK(fake.system.notify(&fake, "hello", 5).status == SP5_OK);
    CHECK(fake.system.notify(&fake, "hello", SIZE_MAX).reason == SP5_REASON_CAPACITY);
    return 0;
}

static int deterministic_and_deadlines(void)
{
    sp5_fake_platform first;
    sp5_fake_platform second;
    sp5_fake_init(&first);
    sp5_fake_init(&second);
    sp5_media_config mode = config();
    CHECK(first.capture.open(&first, &mode).status == SP5_OK);
    CHECK(second.capture.open(&second, &mode).status == SP5_OK);
    sp5_timestamp expired = deadline(&first);
    expired.epoch++;
    sp5_video_frame frame;
    CHECK(first.capture.acquire(&first, expired, true, &frame).reason == SP5_REASON_CLOCK);
    CHECK(sp5_fake_advance(&first, 3).status == SP5_OK);
    expired = first.now;
    expired.ticks--;
    CHECK(first.capture.acquire(&first, expired, true, &frame).reason == SP5_REASON_TIMEOUT);
    CHECK(!first.video_leased);
    CHECK(sp5_fake_advance(&second, 3).status == SP5_OK);
    sp5_video_frame other;
    CHECK(first.capture.acquire(&first, deadline(&first), true, &frame).status == SP5_OK);
    CHECK(second.capture.acquire(&second, deadline(&second), true, &other).status == SP5_OK);
    CHECK(frame.frame_id == other.frame_id && frame.lease_id == other.lease_id);
    CHECK(frame.capture_timestamp.ticks == other.capture_timestamp.ticks);
    CHECK(memcmp(frame.surface, other.surface, sizeof(first.video_samples)) == 0);
    uint16_t samples[8];
    for (size_t i = 0; i < 8; ++i) {
        samples[i] = first.video_samples[i];
    }
    CHECK(sp5_fake_set_source(&first, SP5_PROTECTION_CLEAR, SP5_HDR_PQ_BT2020).status == SP5_OK);
    CHECK(memcmp(frame.surface, samples, sizeof(samples)) == 0);
    CHECK(first.capture.release(&first, frame.lease_id).status == SP5_OK);
    CHECK(first.capture.acquire(&first, deadline(&first), true, &frame).status == SP5_OK);
    CHECK(first.video_samples[1] == 128);
    first.fail_next[SP5_FAKE_CAPTURE_RELEASE] = SP5_REASON_PLATFORM;
    CHECK(first.capture.release(&first, frame.lease_id).reason == SP5_REASON_PLATFORM);
    CHECK(first.video_leased);
    CHECK(first.capture.release(&first, frame.lease_id).status == SP5_OK);
    first.fail_next[SP5_FAKE_CAPTURE_CLOSE] = SP5_REASON_PLATFORM;
    CHECK(first.capture.close(&first).reason == SP5_REASON_PLATFORM && first.capture_open);
    CHECK(first.capture.close(&first).status == SP5_OK);
    mode.dynamic_range = SP5_HDR_PQ_BT2020;
    mode.width = 4;
    uint64_t epoch = first.source.epoch;
    CHECK(first.capture.open(&first, &mode).status == SP5_OK);
    CHECK(first.source.epoch > epoch);
    CHECK(first.capture.close(&first).status == SP5_OK);
    CHECK(second.capture.release(&second, other.lease_id).status == SP5_OK);
    CHECK(second.capture.close(&second).status == SP5_OK);
    return 0;
}

int main(void)
{
    return media() || independent_input() || clocks_and_faults() || deterministic_and_deadlines();
}
