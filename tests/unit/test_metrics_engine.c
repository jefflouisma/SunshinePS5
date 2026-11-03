/* All clocks, frames and delivery observations here are synthetic. */
#include "../fakes/platform.h"
#include "metrics/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            return __LINE__;                                                                       \
        }                                                                                          \
    } while (0)

typedef struct {
    sp5_metrics *metrics;
    size_t lines;
    bool interfere;
    bool fail;
    bool dump;
} export_state;

static sp5_result sink(void *context, sp5_bytes chunk)
{
    export_state *state = context;
    if (state->fail) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    ++state->lines;
    if (state->dump && fwrite(chunk.data, 1, chunk.size, stdout) != chunk.size) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    if (state->interfere) {
        state->interfere = false;
        /* Export must hold no worker lock while a potentially slow sink runs. */
        sp5_frame_record record = {99, 3, {7, 8, 9, 10, 11}, SP5_FRAME_ALL_VALID};
        for (unsigned i = 0; i < 3; ++i) {
            sp5_result result = sp5_metrics_push_frame(state->metrics, &record);
            if (result.status != SP5_OK) {
                return result;
            }
            result = sp5_metrics_worker_step(state->metrics, 3, NULL);
            if (result.status != SP5_OK) {
                return result;
            }
        }
    }
    return sp5_success();
}

static int exercise_export(bool dump)
{
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_metrics *m = sp5_metrics_create(71, (sp5_metrics_capacity){2, 2, 3}, &fake.clock);
    CHECK(m != NULL);
    sp5_metrics_context context = {.host_version = "portable-fixture",
                                   .firmware_version = "synthetic",
                                   .client_name = "synthetic \"client\"\nonly",
                                   .codec = "h264",
                                   .synthetic = true,
                                   .mode = {.width = 1920,
                                            .height = 1080,
                                            .fps_numerator = 60,
                                            .fps_denominator = 1,
                                            .dynamic_range = SP5_SDR,
                                            .chosen_bitrate_kbps = 12000},
                                   .requested_bitrate_kbps = 13000};
    CHECK(sp5_metrics_set_context(m, &context).status == SP5_OK);
    CHECK(sp5_metrics_enable(m, true, 0).status == SP5_OK);
    CHECK(sp5_metrics_set_context(m, &context).reason == SP5_REASON_OWNERSHIP);
    sp5_frame_record record = {3, 1, {0, 1, 65535, 65536, UINT64_MAX}, SP5_FRAME_ALL_VALID};
    sp5_input_record input = {4, 1, 23, 2,
                              SP5_INPUT_VALID | SP5_INPUT_CONFIRMED | SP5_INPUT_AUTHENTICATED};
    CHECK(sp5_metrics_push_frame(m, &record).status == SP5_OK);
    CHECK(sp5_metrics_push_input(m, &input).status == SP5_OK);
    CHECK(sp5_metrics_count_stream(
              m, (sp5_metrics_stream_event){SP5_FRAME_NETWORK_SUBMITTED, 100, 120})
              .status == SP5_OK);
    CHECK(sp5_metrics_count_client(m, 9, 3).status == SP5_OK);
    CHECK(sp5_metrics_worker_step(m, 2, NULL).status == SP5_OK);
    export_state state = {.metrics = m, .dump = dump};
    sp5_metrics_sink output = {&state, sink};
    CHECK(sp5_metrics_export(m, &output).status == SP5_OK && state.lines == 3);
    if (!dump) {
        state.fail = true;
        CHECK(sp5_metrics_export(m, &output).reason == SP5_REASON_PLATFORM);
        state.fail = false;
        state.interfere = true;
        CHECK(sp5_metrics_export(m, &output).reason == SP5_REASON_COVERAGE_GAP);
        CHECK(sp5_metrics_export(m, &output).status == SP5_OK);
        CHECK(sp5_metrics_enable(m, false, 3).status == SP5_OK);
        CHECK(sp5_metrics_count_stream(m, (sp5_metrics_stream_event){SP5_FRAME_HOST_DROPPED, 5, 7})
                  .status == SP5_OK);
        CHECK(sp5_metrics_worker_step(m, 10000000, NULL).status == SP5_OK);
        sp5_metrics_view view;
        CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK);
        CHECK(view.whole[0].p99.population == 4 && view.continuous.encoded_bytes == 105);
        CHECK(view.continuous.frame_outcomes[SP5_FRAME_HOST_DROPPED] == 1);
        CHECK(view.continuous.encoded_bitrate_bps.present &&
              view.continuous.encoded_bitrate_bps.value == 84);
        CHECK(view.continuous.network_throughput_bps.value == 101);
        CHECK(view.continuous.frame_rate_millihz.value == 100);
        CHECK(view.continuous.client_lost_packets == 9 &&
              view.continuous.client_recovery_requests == 3);
    }
    sp5_metrics_destroy(m);
    return 0;
}

static int clocks_and_bounds(void)
{
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_metrics *m = sp5_metrics_create(2, (sp5_metrics_capacity){4, 4, 3}, &fake.clock);
    CHECK(m != NULL && sp5_metrics_enable(m, true, 0).status == SP5_OK);
    for (uint64_t i = 0; i < 90; ++i) {
        sp5_frame_record record = {i, i, {1, 2, 3, 4, 5}, SP5_FRAME_ALL_VALID};
        CHECK(sp5_metrics_push_frame(m, &record).status == SP5_OK);
        CHECK(sp5_metrics_worker_step(m, i, NULL).status == SP5_OK);
    }
    sp5_metrics_view view;
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK && view.whole[0].p99.population == 90);
    CHECK(sp5_metrics_worker_step(m, 88, NULL).reason == SP5_REASON_CLOCK);
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK);
    CHECK(view.coverage_reason == SP5_REASON_CLOCK && !view.whole[0].p99.value_us.present);
    sp5_metrics_destroy(m);
    m = sp5_metrics_create(3, (sp5_metrics_capacity){4, 4, 3}, &fake.clock);
    CHECK(m != NULL);
    for (uint64_t i = 0; i < 130; ++i) {
        sp5_result result = sp5_metrics_enable(m, (i % 2) == 0, i);
        CHECK(i < 127 ? result.status == SP5_OK : result.reason == SP5_REASON_CAPACITY);
    }
    CHECK(sp5_metrics_worker_step(m, 130, NULL).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK);
    CHECK(view.interval_count == 128 && view.coverage_validity == SP5_VALIDITY_INVALID);
    m->whole[0].missing = UINT64_MAX;
    atomic_store(&m->frames.lost, 1);
    CHECK(sp5_metrics_worker_step(m, 131, NULL).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK &&
          view.coverage_reason == SP5_REASON_OVERFLOW);
    CHECK(!view.whole[0].p99.value_us.present);
    sp5_metrics_destroy(m);
    return 0;
}

static int rolling_recovery_and_frame_conversion(void)
{
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_metrics *m = sp5_metrics_create(4, (sp5_metrics_capacity){2, 2, 3}, &fake.clock);
    CHECK(m != NULL && sp5_metrics_enable(m, true, 0).status == SP5_OK);
    for (uint64_t i = 0; i < 3; ++i) {
        sp5_frame_record record = {i, i, {1, 2, 3, 4, 5}, SP5_FRAME_ALL_VALID};
        CHECK(sp5_metrics_push_frame(m, &record).status == SP5_OK);
    }
    CHECK(sp5_metrics_worker_step(m, 2, NULL).status == SP5_OK);
    sp5_metrics_view view;
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK && !view.rolling[0].p99.value_us.present);
    CHECK(view.rolling_population[0].missing == 1);
    CHECK(sp5_metrics_worker_step(m, 10000000, NULL).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK);
    CHECK(view.rolling[0].p99.value_us.present && view.rolling[0].p99.population == 2);
    CHECK(view.rolling_population[0].missing == 0);
    CHECK(sp5_metrics_worker_step(m, 600000001, NULL).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK && view.retained_frames == 2);
    CHECK(sp5_metrics_worker_step(m, 600000003, NULL).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK && view.retained_frames == 0);
    sp5_metrics_destroy(m);
    m = sp5_metrics_create(5, (sp5_metrics_capacity){2, 2, 3}, &fake.clock);
    CHECK(m != NULL && sp5_metrics_enable(m, true, 0).status == SP5_OK);
    sp5_frame_evidence evidence = {
        .session_id = 5, .frame_id = 9, .outcome = SP5_FRAME_NETWORK_SUBMITTED};
    for (size_t i = 0; i < 5; ++i) {
        evidence.stages[i] =
            (sp5_stage_span){fake.now, fake.now, SP5_VALIDITY_VALID, SP5_REASON_NONE};
        evidence.stages[i].end.ticks = 10 + i;
    }
    CHECK(sp5_metrics_record_frame(m, &evidence).status == SP5_OK);
    CHECK(sp5_metrics_worker_step(m, 14, NULL).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK &&
          view.whole[4].p99.value_us.value == 14);
    evidence.session_id = 6;
    CHECK(sp5_metrics_record_frame(m, &evidence).reason == SP5_REASON_OWNERSHIP);
    sp5_metrics_destroy(m);
    m = sp5_metrics_create(6, (sp5_metrics_capacity){2, 2, 3}, &fake.clock);
    CHECK(m != NULL && sp5_metrics_enable(m, true, 0).status == SP5_OK);
    evidence.stages[1].end.epoch = 2;
    CHECK(sp5_metrics_record_frame(m, &evidence).status == SP5_OK);
    CHECK(sp5_metrics_worker_step(m, 14, NULL).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK);
    CHECK(view.whole[0].p99.value_us.present && !view.whole[1].p99.value_us.present);
    CHECK(view.whole_population[1].missing == 1);
    CHECK(view.whole[1].p99.reason == SP5_REASON_CLOCK);
    sp5_metrics_destroy(m);
    return 0;
}

typedef struct {
    sp5_metrics *metrics;
    atomic_bool done;
    atomic_bool failed;
    bool input;
} producer_state;

static void *produce(void *context)
{
    producer_state *state = context;
    for (uint64_t i = 0; i < 2000; ++i) {
        sp5_frame_record frame = {i, i, {1, 2, 3, 4, 5}, SP5_FRAME_ALL_VALID};
        sp5_input_record event = {i, i, 7, 1, SP5_INPUT_VALID | SP5_INPUT_CONFIRMED};
        sp5_result result = state->input ? sp5_metrics_push_input(state->metrics, &event)
                                         : sp5_metrics_push_frame(state->metrics, &frame);
        if (result.status != SP5_OK && result.reason != SP5_REASON_CAPACITY) {
            atomic_store(&state->failed, true);
        }
        result = state->input ? sp5_metrics_count_client(state->metrics, 1, 2)
                              : sp5_metrics_count_stream(
                                    state->metrics,
                                    (sp5_metrics_stream_event){SP5_FRAME_NETWORK_SUBMITTED, 3, 4});
        if (result.status != SP5_OK) {
            atomic_store(&state->failed, true);
        }
    }
    atomic_store(&state->done, true);
    return NULL;
}

static int concurrent_producers(void)
{
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_metrics *m = sp5_metrics_create(7, (sp5_metrics_capacity){64, 64, 3}, &fake.clock);
    CHECK(m != NULL && sp5_metrics_enable(m, true, 0).status == SP5_OK);
    producer_state video = {.metrics = m}, input = {.metrics = m, .input = true};
    atomic_init(&video.done, false);
    atomic_init(&video.failed, false);
    atomic_init(&input.done, false);
    atomic_init(&input.failed, false);
    pthread_t frame_thread, input_thread;
    CHECK(pthread_create(&frame_thread, NULL, produce, &video) == 0);
    CHECK(pthread_create(&input_thread, NULL, produce, &input) == 0);
    do {
        CHECK(sp5_metrics_worker_step(m, 2000, NULL).status == SP5_OK);
    } while (!atomic_load(&video.done) || !atomic_load(&input.done));
    CHECK(pthread_join(frame_thread, NULL) == 0 && pthread_join(input_thread, NULL) == 0);
    CHECK(!atomic_load(&video.failed) && !atomic_load(&input.failed));
    CHECK(sp5_metrics_worker_step(m, 2000, NULL).status == SP5_OK);
    sp5_metrics_view view;
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK);
    CHECK(view.whole[0].p99.population + view.lost_frames == 2000);
    CHECK(view.whole[5].p99.population + view.lost_inputs == 2000);
    CHECK(view.continuous.encoded_bytes == 6000 && view.continuous.network_bytes == 8000);
    CHECK(view.continuous.client_lost_packets == 2000 &&
          view.continuous.client_recovery_requests == 4000);
    CHECK(view.continuous.validity == SP5_VALIDITY_VALID);
    sp5_metrics_destroy(m);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "dump") == 0) {
        return exercise_export(true);
    }
    int result = exercise_export(false);
    if (result == 0) {
        result = clocks_and_bounds();
    }
    if (result == 0) {
        result = rolling_recovery_and_frame_conversion();
    }
    if (result == 0) {
        result = concurrent_producers();
    }
    char error[80];
    int length = snprintf(error, sizeof(error), "metrics engine failure at line %d\n", result);
    if (result != 0 &&
        (length < 0 || (size_t)length >= sizeof(error) || fputs(error, stderr) == EOF)) {
        return 2;
    }
    return result != 0;
}
