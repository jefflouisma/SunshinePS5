/* Synthetic contracts for real metrics engine + management extensions. */
#include "../fakes/platform.h"
#include "metrics/internal.h"
#include "web/diagnostics.h"
#include "web/metrics.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs(#c "\n", stderr) == EOF)                                                     \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
typedef struct {
    char bytes[262144];
    size_t used, calls;
    bool fail;
} capture;
static sp5_result capture_write(void *context, sp5_bytes chunk)
{
    capture *c = context;
    ++c->calls;
    if (c->fail)
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    if (chunk.size > sizeof(c->bytes) - c->used - 1)
        return sp5_failure(SP5_REASON_CAPACITY);
    memcpy(c->bytes + c->used, chunk.data, chunk.size);
    c->used += chunk.size;
    c->bytes[c->used] = 0;
    return sp5_success();
}
static sp5_metrics *engine(sp5_fake_platform *fake)
{
    sp5_fake_init(fake);
    sp5_metrics *m = sp5_metrics_create(99, (sp5_metrics_capacity){16, 16, 8}, &fake->clock);
    if (m == NULL)
        return NULL;
    sp5_metrics_context context = {.synthetic = true,
                                   .mode = {1280, 720, 60000, 1001, NULL, SP5_SDR, 20000},
                                   .requested_bitrate_kbps = 25000};
    (void)snprintf(context.host_version, sizeof(context.host_version), "unit-host");
    (void)snprintf(context.firmware_version, sizeof(context.firmware_version), "synthetic");
    (void)snprintf(context.client_name, sizeof(context.client_name), "device <script>");
    (void)snprintf(context.codec, sizeof(context.codec), "h264");
    if (sp5_metrics_set_context(m, &context).status != SP5_OK) {
        sp5_metrics_destroy(m);
        return NULL;
    }
    return m;
}
static int contracts(void)
{
    sp5_fake_platform fake;
    sp5_metrics *m = engine(&fake);
    CHECK(m != NULL);
    sp5_metrics_web web;
    CHECK(sp5_metrics_web_init(&web, m, false, 0).status == SP5_OK);
    CHECK(!sp5_metrics_web_effective(&web) && !sp5_metrics_enabled(m));
    uint8_t data[65536];
    sp5_web_response response = {.body = {data, sizeof(data), 0}};
    sp5_request request = {.method = {(const uint8_t *)"GET", 3},
                           .target = {(const uint8_t *)"/api/v1/metrics", 15}};
    CHECK(sp5_metrics_web_route(&web, &request, 0, &response));
    CHECK(response.status == 200 && response.body.size < sizeof(data));
    data[response.body.size] = 0;
    CHECK(strstr((char *)data, "\"requested\":false") != NULL);
    CHECK(strstr((char *)data, "\"effective\":false") != NULL);
    CHECK(strstr((char *)data, "\"value_us\":null") != NULL);
    sp5_metrics_web_requested(&web, true, 1);
    CHECK(!sp5_metrics_web_effective(&web) && !sp5_metrics_enabled(m));
    CHECK(sp5_metrics_web_worker_step(&web, 500000).status == SP5_OK);
    CHECK(sp5_metrics_web_effective(&web) && sp5_metrics_enabled(m));
    sp5_frame_record frame = {.frame_id = 1,
                              .observed_us = 500001,
                              .duration_us = {100, 200, 200, 50, 20000},
                              .flags = SP5_FRAME_ALL_VALID};
    CHECK(sp5_metrics_push_frame(m, &frame).status == SP5_OK);
    CHECK(sp5_metrics_web_worker_step(&web, 1000000).status == SP5_OK);
    sp5_metrics_web_requested(&web, false, 1000001);
    CHECK(sp5_metrics_web_worker_step(&web, 1500000).status == SP5_OK);
    CHECK(!sp5_metrics_web_effective(&web) && !sp5_metrics_enabled(m));
    sp5_metrics_web_requested(&web, true, 1500001);
    CHECK(sp5_metrics_web_worker_step(&web, 2000000).status == SP5_OK);
    frame.frame_id = 2;
    frame.observed_us = 2000001;
    frame.duration_us[0] = 70000;
    CHECK(sp5_metrics_push_frame(m, &frame).status == SP5_OK);
    CHECK(sp5_metrics_web_worker_step(&web, 2500000).status == SP5_OK);
    sp5_metrics_view view;
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK);
    sp5_buffer output = {data, sizeof(data), 0};
    CHECK(sp5_metrics_snapshot_json(&view, &output).status == SP5_OK);
    data[output.size] = 0;
    CHECK(strstr((char *)data, "\"lower_bound_us\":65536") != NULL);
    CHECK(strstr((char *)data, "\"requested_bitrate_kbps\":25000") != NULL);
    CHECK(strstr((char *)data, "\"chosen_bitrate_kbps\":20000") != NULL);
    CHECK(strstr((char *)data, "lowered_to_maximum") != NULL);
    CHECK(strstr((char *)data, "metrics_disabled") != NULL);
    CHECK(strstr((char *)data, "last_10_seconds") != NULL &&
          strstr((char *)data, "whole_session") != NULL);
    CHECK(strstr((char *)data, "client_recovery_requests") != NULL);
    uint8_t small[4];
    sp5_buffer tiny = {small, sizeof(small), 0};
    CHECK(sp5_metrics_snapshot_json(&view, &tiny).reason == SP5_REASON_CAPACITY && tiny.size == 0);
    capture c = {0};
    sp5_metrics_sink sink = {&c, capture_write};
    CHECK(sp5_metrics_web_export(&web, &sink).status == SP5_OK);
    CHECK(c.calls == 3 && strstr(c.bytes, "\"kind\":\"frame\"") != NULL);
    c.fail = true;
    CHECK(sp5_metrics_web_export(&web, &sink).reason == SP5_REASON_WOULD_BLOCK);
    c.fail = false;
    c.used = 0;
    c.calls = 0;
    sp5_diagnostics diagnostics;
    CHECK(sp5_diagnostics_init(&diagnostics, m, "unit-host", "synthetic").status == SP5_OK);
    CHECK(
        sp5_diagnostics_log(&diagnostics, 1, 99, SP5_LOG_DIAGNOSTIC, SP5_REASON_PLATFORM).status ==
        SP5_OK);
    CHECK(sp5_diagnostics_end(&diagnostics,
                              (sp5_session_end_record){99, SP5_END_REVOKED, SP5_REASON_NONE})
              .status == SP5_OK);
    CHECK(sp5_diagnostics_end(
              &diagnostics, (sp5_session_end_record){100, SP5_END_CLIENT_LOST, SP5_REASON_TIMEOUT})
              .status == SP5_OK);
    CHECK(sp5_diagnostics_end(&diagnostics,
                              (sp5_session_end_record){99, SP5_END_REVOKED, SP5_REASON_NONE})
              .status == SP5_OK);
    CHECK(diagnostics.end_count == 2);
    CHECK(sp5_diagnostics_end(&diagnostics,
                              (sp5_session_end_record){99, SP5_END_ERROR, SP5_REASON_PLATFORM})
              .reason == SP5_REASON_OWNERSHIP);
    CHECK(sp5_diagnostics_export(&diagnostics, &sink).status == SP5_OK);
    CHECK(strstr(c.bytes, "revoked") != NULL && strstr(c.bytes, "client_lost") != NULL);
    CHECK(strstr(c.bytes, "private_key") == NULL && strstr(c.bytes, "PIN") == NULL &&
          strstr(c.bytes, "token") == NULL);
    CHECK(strstr(c.bytes, "unit-host") != NULL && strstr(c.bytes, "metrics_coverage") != NULL);
    for (unsigned i = 0; i < SP5_DIAGNOSTIC_LOG_CAPACITY + 4; ++i)
        CHECK(sp5_diagnostics_log(&diagnostics, i, 99, SP5_LOG_DIAGNOSTIC, SP5_REASON_TIMEOUT)
                  .status == SP5_OK);
    CHECK(diagnostics.log_count == SP5_DIAGNOSTIC_LOG_CAPACITY && diagnostics.logs_evicted == 5);
    for (unsigned i = 0; i < SP5_DIAGNOSTIC_END_CAPACITY + 4; ++i)
        CHECK(sp5_diagnostics_end(&diagnostics, (sp5_session_end_record){i + 1, SP5_END_CLIENT_QUIT,
                                                                         SP5_REASON_NONE})
                  .status == SP5_OK);
    CHECK(diagnostics.end_count == SP5_DIAGNOSTIC_END_CAPACITY && diagnostics.ends_evicted == 6);
    c.used = 0;
    c.calls = 0;
    CHECK(sp5_diagnostics_export(&diagnostics, &sink).status == SP5_OK);
    CHECK(strstr(c.bytes, "\"logs_evicted\":5") != NULL &&
          strstr(c.bytes, "\"end_reasons_evicted\":6") != NULL);
    c.fail = true;
    CHECK(sp5_diagnostics_export(&diagnostics, &sink).reason == SP5_REASON_WOULD_BLOCK);
    CHECK(sp5_diagnostics_end(&diagnostics,
                              (sp5_session_end_record){0, SP5_END_REVOKED, SP5_REASON_NONE})
              .reason == SP5_REASON_INVALID_ARGUMENT);

    CHECK(sp5_metrics_web_worker_step(&web, 603000000).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK && view.retained_frames == 0);
    CHECK(view.whole_population[0].seen == 2 && view.whole[0].p99.lower_bound_us.present);
    sp5_metrics_destroy(m);
    return 0;
}
typedef struct {
    sp5_metrics *m;
    atomic_bool failed;
} readers;
static void *read_snapshots(void *context)
{
    readers *r = context;
    for (unsigned i = 0; i < 20; ++i) {
        sp5_metrics_view view;
        uint8_t json[65536];
        sp5_buffer output = {json, sizeof(json), 0};
        sp5_result result = sp5_metrics_snapshot(r->m, &view);
        if (result.reason == SP5_REASON_WOULD_BLOCK)
            continue;
        if (result.status != SP5_OK || sp5_metrics_snapshot_json(&view, &output).status != SP5_OK)
            atomic_store(&r->failed, true);
    }
    return NULL;
}
static int concurrent(void)
{
    sp5_fake_platform fake;
    sp5_metrics *m = engine(&fake);
    CHECK(m != NULL);
    readers r = {.m = m};
    atomic_init(&r.failed, false);
    pthread_t a, b;
    CHECK(pthread_create(&a, NULL, read_snapshots, &r) == 0 &&
          pthread_create(&b, NULL, read_snapshots, &r) == 0);
    for (unsigned i = 0; i < 20; ++i) {
        sp5_result result = sp5_metrics_worker_step(m, i, NULL);
        CHECK(result.status == SP5_OK || result.reason == SP5_REASON_WOULD_BLOCK);
    }
    CHECK(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0 && !atomic_load(&r.failed));
    sp5_metrics_destroy(m);
    return 0;
}
/* FR-048: a management view or download that meets an in-progress worker pass
 * must still be served, never reported as a failed view or truncated file. The
 * test holds the engine lock exactly as a worker pass does. Yielding only gives
 * an early failing return the chance to happen; it is not the oracle. */
typedef struct {
    sp5_metrics_web *web;
    bool exporting;
    atomic_bool started;
    unsigned status;
    uint8_t body[65536];
    size_t body_size;
    capture output;
} locked_reader;
static void *read_during_worker_pass(void *context)
{
    locked_reader *r = context;
    atomic_store_explicit(&r->started, true, memory_order_release);
    if (r->exporting) {
        sp5_metrics_sink sink = {&r->output, capture_write};
        r->status = sp5_metrics_web_export(r->web, &sink).status == SP5_OK ? 200 : 0;
        return NULL;
    }
    sp5_web_response response = {.body = {r->body, sizeof(r->body) - 1, 0}};
    sp5_request request = {.method = {(const uint8_t *)"GET", 3},
                           .target = {(const uint8_t *)"/api/v1/metrics", 15}};
    r->status = sp5_metrics_web_route(r->web, &request, 0, &response) ? response.status : 0;
    r->body_size = response.body.size;
    r->body[r->body_size] = 0;
    return NULL;
}
static locked_reader reader;
static int readers_wait_for_worker_pass(void)
{
    for (unsigned exporting = 0; exporting < 2; ++exporting) {
        sp5_fake_platform fake;
        sp5_metrics *m = engine(&fake);
        CHECK(m != NULL);
        sp5_metrics_web web;
        CHECK(sp5_metrics_web_init(&web, m, true, 0).status == SP5_OK);
        CHECK(sp5_metrics_web_worker_step(&web, 500000).status == SP5_OK);
        sp5_frame_record frame = {.frame_id = 1,
                                  .observed_us = 500001,
                                  .duration_us = {100, 200, 200, 50, 20000},
                                  .flags = SP5_FRAME_ALL_VALID};
        CHECK(sp5_metrics_push_frame(m, &frame).status == SP5_OK);
        CHECK(sp5_metrics_web_worker_step(&web, 1000000).status == SP5_OK);
        sp5_metrics_view view;
        CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK && view.retained_frames == 1 &&
              view.retained_inputs == 0);
        memset(&reader, 0, sizeof(reader));
        reader.web = &web;
        reader.exporting = exporting != 0;
        atomic_init(&reader.started, false);
        CHECK(pthread_mutex_lock(&m->state_lock) == 0);
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, read_during_worker_pass, &reader) == 0);
        while (!atomic_load_explicit(&reader.started, memory_order_acquire))
            sched_yield();
        for (unsigned i = 0; i < 10000; ++i)
            sched_yield();
        CHECK(pthread_mutex_unlock(&m->state_lock) == 0);
        CHECK(pthread_join(thread, NULL) == 0);
        CHECK(reader.status == 200);
        if (exporting) {
            /* One coverage header plus every retained record: nothing truncated. */
            CHECK(reader.output.calls == 1 + view.retained_frames &&
                  strstr(reader.output.bytes, "\"kind\":\"metrics_coverage\"") != NULL &&
                  strstr(reader.output.bytes, "\"kind\":\"frame\"") != NULL);
        } else {
            CHECK(strstr((char *)reader.body, "\"effective\":true") != NULL &&
                  strstr((char *)reader.body, "\"capture\":null") == NULL &&
                  strstr((char *)reader.body, "last_10_seconds") != NULL);
        }
        sp5_metrics_destroy(m);
    }
    return 0;
}
static bool timing_requested;
static sp5_result (*original_deliver)(void *, const sp5_controller_state *, bool,
                                      sp5_input_evidence *);
static sp5_result observe_timing(void *context, const sp5_controller_state *state, bool timing,
                                 sp5_input_evidence *event)
{
    timing_requested = timing;
    return original_deliver(context, state, timing, event);
}
static int lifecycle_toggle(void)
{
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    fake.confirm_delivery = true;
    original_deliver = fake.input.deliver;
    sp5_input_ops input = fake.input;
    input.deliver = observe_timing;
    sp5_lifecycle_ops ops = {fake.capture, fake.encoder, fake.audio, input, fake.system};
    sp5_lifecycle *life = sp5_lifecycle_create(90, &ops);
    CHECK(life != NULL);
    sp5_lifecycle_policy policy = {.mode_count = 1,
                                   .media_bytes = 4096,
                                   .core_bytes = 4096,
                                   .validity = SP5_VALIDITY_VALID,
                                   .synthetic = true,
                                   .evidence_ref = "unit-toggle-only",
                                   .clock = fake.clock,
                                   .metrics_capacity = {16, 16, 8}};
    policy.modes[0] = (sp5_host_mode){2, 2, 60, "synthetic", SP5_SDR, 100, 200};
    policy.metrics_context.synthetic = true;
    CHECK(sp5_lifecycle_configure(life, &policy).status == SP5_OK);
    sp5_media_config media = {2, 2, 60, 1, "synthetic", SP5_SDR, 100};
    CHECK(sp5_lifecycle_start(life, &media, 0).status == SP5_OK);
    sp5_metrics_web web;
    CHECK(sp5_metrics_web_init(&web, sp5_lifecycle_metrics(life), false, 0).status == SP5_OK);
    sp5_control_event packet = {
        .type = 0x0206, .channel = 0x10, .delivery = SP5_CONTROL_RELIABLE, .payload_size = 34};
    const uint8_t pad[34] = {0, 0,   0, 30, 12, 0, 0, 0, 0x1a, 0, 0, 0,    1, 0, 0x14, 0,    0,
                             4, 127, 0, 0,  0,  0, 0, 0, 0,    0, 0, 0x9c, 0, 0, 0,    0x55, 0};
    memcpy(packet.payload, pad, sizeof(pad));
    sp5_input_evidence event;
    CHECK(sp5_lifecycle_input(life, &packet, fake.now, 1, &event).status == SP5_OK &&
          !timing_requested);
    sp5_metrics_web_requested(&web, true, 1);
    CHECK(sp5_metrics_web_worker_step(&web, 1).status == SP5_OK);
    CHECK(sp5_lifecycle_input(life, &packet, fake.now, 2, &event).status == SP5_OK &&
          timing_requested);
    sp5_metrics_web_requested(&web, false, 2);
    CHECK(sp5_metrics_web_worker_step(&web, 2).status == SP5_OK);
    CHECK(sp5_lifecycle_input(life, &packet, fake.now, 3, &event).status == SP5_OK &&
          !timing_requested);
    CHECK(fake.calls[SP5_FAKE_CLOCK_NOW] == 1);
    sp5_lifecycle_destroy(life);
    return 0;
}
int main(void)
{
    return contracts() || concurrent() || readers_wait_for_worker_pass() || lifecycle_toggle();
}
