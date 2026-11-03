/* Virtual-time synthetic integration: no sleeps, console or hardware claims. */
#include "platform.h"
#include "web/metrics.h"
#include <pthread.h>
#include <sched.h>
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
    sp5_metrics *metrics;
    atomic_bool failed;
} readers;
static void *read_view(void *context)
{
    readers *r = context;
    for (unsigned i = 0; i < 100; ++i) {
        sp5_metrics_view view;
        uint8_t bytes[65536];
        sp5_buffer out = {bytes, sizeof(bytes), 0};
        sp5_result result = sp5_metrics_snapshot(r->metrics, &view);
        if (result.reason == SP5_REASON_WOULD_BLOCK)
            continue;
        if (result.status != SP5_OK || sp5_metrics_snapshot_json(&view, &out).status != SP5_OK)
            atomic_store(&r->failed, true);
    }
    return NULL;
}
static int publish(const char *name, const sp5_metrics_view *view)
{
    uint8_t bytes[65536];
    sp5_buffer out = {bytes, sizeof(bytes), 0};
    CHECK(sp5_metrics_snapshot_json(view, &out).status == SP5_OK);
    CHECK(fputs("{\"case\":\"", stdout) != EOF && fputs(name, stdout) != EOF &&
          fputs("\",\"snapshot\":", stdout) != EOF);
    CHECK(fwrite(out.data, 1, out.size, stdout) == out.size && fputs("}\n", stdout) != EOF);
    return 0;
}
int main(void)
{
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_metrics *m = sp5_metrics_create(77, (sp5_metrics_capacity){2048, 2048, 8}, &fake.clock);
    CHECK(m != NULL);
    sp5_metrics_web web;
    CHECK(sp5_metrics_web_init(&web, m, true, 0).status == SP5_OK);
    CHECK(sp5_metrics_web_worker_step(&web, 0).status == SP5_OK);
    for (uint64_t second = 1; second <= 701; ++second) {
        uint64_t at = second * 1000000;
        if (second == 100)
            sp5_metrics_web_requested(&web, false, at);
        if (second == 150)
            sp5_metrics_web_requested(&web, true, at);
        CHECK(sp5_metrics_web_worker_step(&web, at).status == SP5_OK);
        if (sp5_metrics_web_effective(&web)) {
            sp5_frame_record f = {.frame_id = second,
                                  .observed_us = at,
                                  .duration_us = {100, 200, 200, 100, 500},
                                  .flags = SP5_FRAME_ALL_VALID};
            if (second % 2 == 0)
                f.duration_us[0] = 70000;
            CHECK(sp5_metrics_push_frame(m, &f).status == SP5_OK);
        }
        CHECK(sp5_metrics_web_worker_step(&web, at).status == SP5_OK);
    }
    sp5_metrics_view view;
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK);
    CHECK(view.whole_population[0].seen == 651 && view.retained_frames == 552);
    CHECK(view.first_frame_us.value == 150000000 && view.last_frame_us.value == 701000000);
    CHECK(!view.whole[0].p99.value_us.present && view.whole[0].p99.lower_bound_us.value == 65536);
    CHECK(view.disabled_intervals >= 2 && view.enabled_intervals == 2);
    CHECK(publish("long_gap_overflow", &view) == 0);
    unsigned lost = 0;
    for (unsigned i = 0; i < 20; ++i) {
        sp5_frame_record f = {.frame_id = 702 + i,
                              .observed_us = 702000000,
                              .duration_us = {100, 100, 100, 100, 500},
                              .flags = SP5_FRAME_ALL_VALID};
        sp5_result result = sp5_metrics_push_frame(m, &f);
        if (result.reason == SP5_REASON_CAPACITY)
            ++lost;
        else
            CHECK(result.status == SP5_OK);
    }
    CHECK(lost == 12 && sp5_metrics_web_worker_step(&web, 702000000).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK && view.lost_frames == 12);
    CHECK(view.whole_population[0].seen == 671 && !view.rolling[0].p99.value_us.present);
    CHECK(view.rolling[0].p99.reason == SP5_REASON_COVERAGE_GAP);
    CHECK(publish("queue_gap", &view) == 0);
    readers state = {.metrics = m};
    atomic_init(&state.failed, false);
    pthread_t a, b;
    CHECK(pthread_create(&a, NULL, read_view, &state) == 0 &&
          pthread_create(&b, NULL, read_view, &state) == 0);
    for (uint64_t second = 703; second <= 720; ++second) {
        sp5_frame_record f = {.frame_id = second + 100,
                              .observed_us = second * 1000000,
                              .duration_us = {100, 100, 100, 100, 500},
                              .flags = SP5_FRAME_ALL_VALID};
        /* Readers may hold the state lock, making worker steps WOULD_BLOCK;
         * drain the queue first so a scheduling run cannot fill it. */
        sp5_result drained;
        while ((drained = sp5_metrics_web_worker_step(&web, second * 1000000)).reason ==
               SP5_REASON_WOULD_BLOCK)
            sched_yield();
        CHECK(drained.status == SP5_OK);
        CHECK(sp5_metrics_push_frame(m, &f).status == SP5_OK);
        sp5_result result = sp5_metrics_web_worker_step(&web, second * 1000000);
        CHECK(result.status == SP5_OK || result.reason == SP5_REASON_WOULD_BLOCK);
    }
    CHECK(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0 && !atomic_load(&state.failed));
    CHECK(sp5_metrics_web_worker_step(&web, 720000000).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK && view.whole_population[0].seen == 689);
    CHECK(view.rolling[0].p99.value_us.present && view.rolling[0].p99.value_us.value == 100);
    CHECK(publish("concurrent_recovered_window", &view) == 0);
    sp5_metrics_destroy(m);
    m = sp5_metrics_create(78, (sp5_metrics_capacity){2, 2, 8}, &fake.clock);
    CHECK(m != NULL);
    CHECK(sp5_metrics_web_init(&web, m, true, 0).status == SP5_OK &&
          sp5_metrics_web_worker_step(&web, 0).status == SP5_OK);
    for (uint64_t i = 1; i <= 3; ++i) {
        sp5_frame_record f = {.frame_id = i,
                              .observed_us = i * 1000000,
                              .duration_us = {100, 100, 100, 100, 500},
                              .flags = SP5_FRAME_ALL_VALID};
        CHECK(sp5_metrics_push_frame(m, &f).status == SP5_OK &&
              sp5_metrics_web_worker_step(&web, i * 1000000).status == SP5_OK);
    }
    CHECK(sp5_metrics_snapshot(m, &view).status == SP5_OK && view.whole_population[0].seen == 3 &&
          view.retained_frames == 2);
    CHECK(!view.rolling[0].p99.value_us.present &&
          view.rolling[0].p99.reason == SP5_REASON_COVERAGE_GAP);
    CHECK(publish("raw_capacity_gap", &view) == 0);
    sp5_metrics_destroy(m);
    return 0;
}
