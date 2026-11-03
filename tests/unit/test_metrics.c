/* Synthetic portable contract tests; no console or interoperability evidence. */
#if defined(SP5_PENDING_CORE) && defined(__APPLE__)
#define SP5_CORE_API __attribute__((weak_import))
#elif defined(SP5_PENDING_CORE)
#define SP5_CORE_API __attribute__((weak))
#endif

#include "../fakes/platform.h"
#include "metrics.h"
#include <pthread.h>
#include <stdatomic.h>

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            if (fputs(__FILE__ ": " #condition "\n", stderr) == EOF) {                             \
                return 2;                                                                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#ifdef SP5_PENDING_CORE
#define NEED(api)                                                                                  \
    do {                                                                                           \
        if ((api) == NULL) {                                                                       \
            if (fputs("TDD missing implementation: " #api "\n", stderr) == EOF) {                  \
                return 2;                                                                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#else
#define NEED(api) ((void)0)
#endif

/* Fixed raw layouts are part of the portable data-model contract. */
_Static_assert(sizeof(sp5_frame_record) == 64, "data model frame record size");
_Static_assert(sizeof(sp5_input_record) == 32, "data model input record size");

static int percentiles(void)
{
    NEED(sp5_histogram_init);
    NEED(sp5_histogram_add);
    NEED(sp5_histogram_percentile);
    sp5_histogram histogram;
    sp5_histogram_init(&histogram);
    CHECK(!sp5_histogram_percentile(&histogram, 50).value_us.present);
    /* Deliberately unsorted, unique recorded values forbid interpolation. */
    const uint64_t values[] = {7,  1, 20, 2, 19, 3, 18, 4,  17, 5,
                               16, 6, 15, 8, 14, 9, 13, 10, 12, 11};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        CHECK(sp5_histogram_add(&histogram, values[i]).status == SP5_OK);
    }
    sp5_quantile median = sp5_histogram_percentile(&histogram, 50);
    sp5_quantile p95 = sp5_histogram_percentile(&histogram, 95);
    sp5_quantile p99 = sp5_histogram_percentile(&histogram, 99);
    CHECK(median.value_us.present && median.value_us.value == 10 && median.population == 20);
    CHECK(p95.value_us.present && p95.value_us.value == 19);
    CHECK(p99.value_us.present && p99.value_us.value == 20 && p99.validity == SP5_VALIDITY_VALID);
    CHECK(!sp5_histogram_percentile(&histogram, 0).value_us.present);
    CHECK(!sp5_histogram_percentile(&histogram, 101).value_us.present);
    sp5_histogram_init(&histogram);
    CHECK(sp5_histogram_add(&histogram, 0).status == SP5_OK);
    CHECK(sp5_histogram_percentile(&histogram, 99).value_us.present);
    CHECK(sp5_histogram_percentile(&histogram, 99).value_us.value == 0);
    return 0;
}

static int overflow(void)
{
    NEED(sp5_histogram_init);
    NEED(sp5_histogram_add);
    NEED(sp5_histogram_percentile);
    sp5_histogram histogram;
    sp5_histogram_init(&histogram);
    for (unsigned i = 0; i < 98; ++i) {
        CHECK(sp5_histogram_add(&histogram, 65535).status == SP5_OK);
    }
    CHECK(sp5_histogram_add(&histogram, 65536).status == SP5_OK);
    CHECK(sp5_histogram_add(&histogram, UINT64_MAX).status == SP5_OK);
    sp5_quantile p95 = sp5_histogram_percentile(&histogram, 95);
    sp5_quantile p99 = sp5_histogram_percentile(&histogram, 99);
    CHECK(p95.value_us.present && p95.value_us.value == 65535 && p95.population == 100);
    CHECK(!p99.value_us.present && p99.lower_bound_us.present);
    CHECK(p99.lower_bound_us.value == 65536 && p99.population == 100);
    CHECK(histogram.overflow == 2 && histogram.bins[65535] == 98);
    /* The rank includes overflow, rather than selecting from only 98 bins. */
    CHECK(!sp5_histogram_percentile(&histogram, 100).value_us.present);
    return 0;
}

static int counters_and_thresholds(void)
{
    NEED(sp5_histogram_init);
    NEED(sp5_histogram_add);
    NEED(sp5_histogram_percentile);
    NEED(sp5_latency_below_frame_interval);
    sp5_histogram histogram;
    sp5_histogram_init(&histogram);
    histogram.count = UINT64_MAX;
    histogram.bins[5] = UINT64_MAX;
    CHECK(sp5_histogram_add(&histogram, 5).reason == SP5_REASON_OVERFLOW);
    CHECK(histogram.count == UINT64_MAX && histogram.bins[5] == UINT64_MAX);
    CHECK(!sp5_histogram_percentile(&histogram, 99).value_us.present);
    sp5_histogram_init(&histogram);
    CHECK(sp5_histogram_add(&histogram, 1).status == SP5_OK);
    histogram.missing = 1;
    CHECK(!sp5_histogram_percentile(&histogram, 99).value_us.present);
    CHECK(sp5_latency_below_frame_interval(16666, 60, 1));
    CHECK(!sp5_latency_below_frame_interval(16667, 60, 1));
    CHECK(sp5_latency_below_frame_interval(16683, 60000, 1001));
    CHECK(!sp5_latency_below_frame_interval(16684, 60000, 1001));
    CHECK(!sp5_latency_below_frame_interval(1000, 1000, 1)); /* exact equality */
    CHECK(!sp5_latency_below_frame_interval(UINT64_MAX, UINT32_MAX, 1));
    CHECK(!sp5_latency_below_frame_interval(0, 0, 1));
    CHECK(!sp5_latency_below_frame_interval(0, 60, 0));
    return 0;
}

static sp5_frame_record frame(uint64_t id, uint64_t at, uint64_t duration)
{
    return (sp5_frame_record){id,
                              at,
                              {duration, duration + 1, duration + 2, duration + 3, duration + 4},
                              SP5_FRAME_ALL_VALID};
}

static sp5_input_record input(uint64_t id, uint64_t at, uint64_t duration)
{
    return (sp5_input_record){id, at, duration, 0, SP5_INPUT_VALID | SP5_INPUT_CONFIRMED};
}

static int engine_apis(void)
{
    NEED(sp5_metrics_create);
    NEED(sp5_metrics_destroy);
    NEED(sp5_metrics_enable);
    NEED(sp5_metrics_push_frame);
    NEED(sp5_metrics_push_input);
    NEED(sp5_metrics_worker_step);
    NEED(sp5_metrics_snapshot);
    return 0;
}

static int rolling(void)
{
    CHECK(engine_apis() == 0);
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_metrics *metrics = sp5_metrics_create(1, (sp5_metrics_capacity){16, 16, 16}, &fake.clock);
    CHECK(metrics != NULL && sp5_metrics_enable(metrics, true, 0).status == SP5_OK);
    const uint64_t times[] = {0, 1, 9999999, 10000000};
    for (size_t i = 0; i < 4; ++i) {
        sp5_frame_record record = frame(i, times[i], (i + 1) * 100);
        sp5_input_record event = input(i, times[i], (i + 1) * 10);
        CHECK(sp5_metrics_push_frame(metrics, &record).status == SP5_OK);
        CHECK(sp5_metrics_push_input(metrics, &event).status == SP5_OK);
    }
    CHECK(sp5_metrics_worker_step(metrics, UINT64_C(10000000), NULL).status == SP5_OK);
    sp5_metrics_view view;
    CHECK(sp5_metrics_snapshot(metrics, &view).status == SP5_OK);
    for (size_t stage = 0; stage < 5; ++stage) {
        CHECK(view.whole[stage].median.population == 4);
        CHECK(view.whole[stage].median.value_us.value == 200 + stage);
        CHECK(view.rolling[stage].median.population == 3);
        CHECK(view.rolling[stage].median.value_us.value == 300 + stage);
    }
    CHECK(view.rolling[SP5_STAGE_INPUT_DELIVERY].median.value_us.value == 30);
    CHECK(sp5_metrics_worker_step(metrics, UINT64_C(10000001), NULL).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(metrics, &view).status == SP5_OK);
    CHECK(view.rolling[0].median.population == 2 && view.rolling[0].median.value_us.value == 300);
    CHECK(sp5_metrics_worker_step(metrics, UINT64_C(20000000), NULL).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(metrics, &view).status == SP5_OK);
    CHECK(!view.rolling[0].p99.value_us.present && view.whole[0].p99.value_us.value == 400);
    sp5_metrics_destroy(metrics);
    return 0;
}

static int retention(void)
{
    CHECK(engine_apis() == 0);
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_metrics *metrics = sp5_metrics_create(7, (sp5_metrics_capacity){4, 4, 4}, &fake.clock);
    CHECK(metrics != NULL && sp5_metrics_enable(metrics, true, 0).status == SP5_OK);
    sp5_frame_record record = frame(1, 0, 777);
    sp5_input_record event = input(1, 0, 333);
    CHECK(sp5_metrics_push_frame(metrics, &record).status == SP5_OK);
    CHECK(sp5_metrics_push_input(metrics, &event).status == SP5_OK);
    CHECK(sp5_metrics_worker_step(metrics, UINT64_C(600000001), NULL).status == SP5_OK);
    sp5_metrics_view view;
    CHECK(sp5_metrics_snapshot(metrics, &view).status == SP5_OK);
    CHECK(view.retained_frames == 0 && view.retained_inputs == 0);
    CHECK(!view.first_frame_us.present && !view.first_input_us.present);
    CHECK(view.whole[0].p99.value_us.present && view.whole[0].p99.value_us.value == 777);
    CHECK(view.whole[SP5_STAGE_INPUT_DELIVERY].p99.value_us.value == 333);
    CHECK(!view.rolling[0].p99.value_us.present);
    sp5_metrics_destroy(metrics);
    metrics = sp5_metrics_create(8, (sp5_metrics_capacity){2, 2, 4}, &fake.clock);
    CHECK(metrics != NULL && sp5_metrics_enable(metrics, true, 0).status == SP5_OK);
    for (uint64_t i = 0; i < 4; ++i) {
        record = frame(i, i, i + 1);
        CHECK(sp5_metrics_push_frame(metrics, &record).status == SP5_OK);
        CHECK(sp5_metrics_worker_step(metrics, i, NULL).status == SP5_OK);
    }
    CHECK(sp5_metrics_snapshot(metrics, &view).status == SP5_OK);
    CHECK(view.session_id == 8 && view.whole[0].p99.population == 4);
    CHECK(view.whole[0].p99.value_us.value == 4); /* aggregate before eviction */
    CHECK(view.retained_frames == 2 && view.first_frame_us.present &&
          view.first_frame_us.value == 2);
    CHECK(view.last_frame_us.present && view.last_frame_us.value == 3);
    CHECK(!view.rolling[0].p99.value_us.present);                      /* exact expiry data lost */
    CHECK(!view.whole[SP5_STAGE_INPUT_DELIVERY].p99.value_us.present); /* session isolation */
    sp5_metrics_destroy(metrics);
    return 0;
}

static int toggles(void)
{
    CHECK(engine_apis() == 0);
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_metrics *metrics = sp5_metrics_create(1, (sp5_metrics_capacity){16, 16, 16}, &fake.clock);
    CHECK(metrics != NULL);
    sp5_metrics_view view;
    CHECK(sp5_metrics_snapshot(metrics, &view).status == SP5_OK && !view.metrics_enabled);
    CHECK(sp5_metrics_enable(metrics, true, 0).status == SP5_OK);
    sp5_frame_record first = frame(1, 1, 11);
    CHECK(sp5_metrics_push_frame(metrics, &first).status == SP5_OK);
    CHECK(sp5_metrics_enable(metrics, false, 2).status == SP5_OK);
    sp5_frame_record disabled = frame(2, 3, 0);
    CHECK(sp5_metrics_push_frame(metrics, &disabled).status == SP5_OK);
    CHECK(sp5_metrics_enable(metrics, true, 4).status == SP5_OK);
    sp5_frame_record second = frame(3, 5, 99);
    CHECK(sp5_metrics_push_frame(metrics, &second).status == SP5_OK);
    CHECK(sp5_metrics_worker_step(metrics, 6, NULL).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(metrics, &view).status == SP5_OK);
    CHECK(view.whole[0].median.population == 2 && view.whole[0].median.value_us.value == 11);
    CHECK(view.whole[0].p99.value_us.value == 99 && view.retained_frames == 2);
    CHECK(view.enabled_intervals == 2 && view.disabled_intervals >= 1);
    CHECK(view.published_us == 6);
    CHECK(sp5_metrics_enable(metrics, false, 7).status == SP5_OK);
    CHECK(sp5_metrics_worker_step(metrics, 1000007, NULL).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(metrics, &view).status == SP5_OK);
    CHECK(view.whole[0].median.population == 2 && !view.metrics_enabled);
    sp5_metrics_destroy(metrics);
    return 0;
}

static int queue_loss(void)
{
    CHECK(engine_apis() == 0);
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_metrics *metrics = sp5_metrics_create(1, (sp5_metrics_capacity){16, 16, 2}, &fake.clock);
    CHECK(metrics != NULL && sp5_metrics_enable(metrics, true, 0).status == SP5_OK);
    sp5_frame_record record = frame(1, 1, 4);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(sp5_metrics_push_frame(metrics, &record).status == SP5_OK);
    }
    CHECK(sp5_metrics_push_frame(metrics, &record).reason == SP5_REASON_CAPACITY);
    /* Video queue exhaustion cannot exhaust the separate input queue. */
    sp5_input_record event = input(1, 1, 9);
    CHECK(sp5_metrics_push_input(metrics, &event).status == SP5_OK);
    CHECK(sp5_metrics_push_input(metrics, &event).status == SP5_OK);
    CHECK(sp5_metrics_push_input(metrics, &event).reason == SP5_REASON_CAPACITY);
    CHECK(sp5_metrics_worker_step(metrics, 2, NULL).status == SP5_OK);
    sp5_metrics_view view;
    CHECK(sp5_metrics_snapshot(metrics, &view).status == SP5_OK);
    CHECK(view.lost_frames == 1 && view.lost_inputs == 1);
    CHECK(view.coverage_validity == SP5_VALIDITY_INVALID &&
          view.coverage_reason == SP5_REASON_COVERAGE_GAP);
    CHECK(!view.whole[0].p99.value_us.present && !view.rolling[0].p99.value_us.present);
    CHECK(!view.whole[SP5_STAGE_INPUT_DELIVERY].p99.value_us.present);
    sp5_metrics_destroy(metrics);
    return 0;
}

static int off_state(void)
{
    CHECK(engine_apis() == 0);
    NEED(sp5_metrics_stamp);
    NEED(sp5_metrics_protocol_header);
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_metrics *metrics = sp5_metrics_create(1, (sp5_metrics_capacity){4, 4, 4}, &fake.clock);
    CHECK(metrics != NULL);
    sp5_timestamp extra;
    uint64_t clock_reads = fake.calls[SP5_FAKE_CLOCK_NOW];
    CHECK(sp5_metrics_stamp(metrics, &extra).reason == SP5_REASON_METRICS_DISABLED);
    CHECK(extra.validity != SP5_VALIDITY_VALID && fake.calls[SP5_FAKE_CLOCK_NOW] == clock_reads);
    CHECK(sp5_fake_advance(&fake, 1550).status == SP5_OK);
    sp5_timestamp capture = fake.now;
    capture.ticks = 0;
    sp5_protocol_latency_evidence protocol;
    CHECK(sp5_metrics_protocol_header(metrics, 1, capture, false, &protocol).status == SP5_OK);
    CHECK(protocol.wire_tenths_ms == 16 && protocol.validity == SP5_VALIDITY_VALID);
    CHECK(fake.calls[SP5_FAKE_CLOCK_NOW] == clock_reads + 1);
    CHECK(sp5_metrics_protocol_header(metrics, 2, capture, true, &protocol).status == SP5_OK);
    CHECK(protocol.wire_tenths_ms == 0 && protocol.validity != SP5_VALIDITY_VALID);
    sp5_frame_record record = frame(1, 1, 10);
    sp5_input_record event = input(1, 1, 10);
    CHECK(sp5_metrics_push_frame(metrics, &record).status == SP5_OK);
    CHECK(sp5_metrics_push_input(metrics, &event).status == SP5_OK);
    clock_reads = fake.calls[SP5_FAKE_CLOCK_NOW];
    CHECK(sp5_metrics_worker_step(metrics, 1550, NULL).status == SP5_OK);
    sp5_metrics_view view;
    CHECK(sp5_metrics_snapshot(metrics, &view).status == SP5_OK);
    CHECK(view.retained_frames == 0 && view.retained_inputs == 0);
    CHECK(!view.whole[0].p99.value_us.present && fake.calls[SP5_FAKE_CLOCK_NOW] == clock_reads);
    CHECK(sp5_metrics_enable(metrics, true, 1550).status == SP5_OK);
    CHECK(sp5_metrics_stamp(metrics, &extra).status == SP5_OK);
    CHECK(extra.validity == SP5_VALIDITY_VALID &&
          fake.calls[SP5_FAKE_CLOCK_NOW] == clock_reads + 1);
    sp5_metrics_destroy(metrics);
    return 0;
}

/* A worker/export callback is deliberately held on another thread. Producers
 * must return while that callback is held; waiting on it trips CTest's timeout.
 * No sleep, speed threshold, or hardware timing is used as the oracle. */
typedef struct {
    sp5_metrics *metrics;
    atomic_bool entered;
    atomic_bool release;
    bool exporting;
    sp5_result result;
} stalled_consumer;

static void stall(stalled_consumer *consumer)
{
    atomic_store_explicit(&consumer->entered, true, memory_order_release);
    while (!atomic_load_explicit(&consumer->release, memory_order_acquire)) {
    }
}

static void on_snapshot(void *context, const sp5_metrics_view *view)
{
    (void)view;
    stall(context);
}

static sp5_result export_write(void *context, sp5_bytes chunk)
{
    if (chunk.size == 0) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    stall(context);
    return sp5_success();
}

static void *consume(void *context)
{
    stalled_consumer *consumer = context;
    if (consumer->exporting) {
        sp5_metrics_sink sink = {consumer, export_write};
        consumer->result = sp5_metrics_export(consumer->metrics, &sink);
    } else {
        sp5_metrics_observer observer = {consumer, on_snapshot};
        consumer->result = sp5_metrics_worker_step(consumer->metrics, 1000000, &observer);
    }
    return NULL;
}

static int nonblocking(void)
{
    CHECK(engine_apis() == 0);
    NEED(sp5_metrics_export);
    for (unsigned exporting = 0; exporting < 2; ++exporting) {
        sp5_fake_platform fake;
        sp5_fake_init(&fake);
        sp5_metrics *metrics =
            sp5_metrics_create(1, (sp5_metrics_capacity){16, 16, 4}, &fake.clock);
        CHECK(metrics != NULL && sp5_metrics_enable(metrics, true, 0).status == SP5_OK);
        sp5_frame_record record = frame(1, 1, 10);
        CHECK(sp5_metrics_push_frame(metrics, &record).status == SP5_OK);
        CHECK(sp5_metrics_worker_step(metrics, 1, NULL).status == SP5_OK);
        stalled_consumer consumer = {.metrics = metrics, .exporting = exporting != 0};
        atomic_init(&consumer.entered, false);
        atomic_init(&consumer.release, false);
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, consume, &consumer) == 0);
        while (!atomic_load_explicit(&consumer.entered, memory_order_acquire)) {
        }
        /* Both calls must finish before the consumer is permitted to continue. */
        sp5_input_record event = input(2, 1000001, 13);
        sp5_result pushed_frame = sp5_metrics_push_frame(metrics, &record);
        sp5_result pushed_input = sp5_metrics_push_input(metrics, &event);
        atomic_store_explicit(&consumer.release, true, memory_order_release);
        CHECK(pthread_join(thread, NULL) == 0);
        CHECK(pushed_frame.status == SP5_OK && pushed_input.status == SP5_OK);
        CHECK(consumer.result.status == SP5_OK);
        sp5_metrics_destroy(metrics);
    }
    return 0;
}

static int limits_and_delivery(void)
{
    CHECK(engine_apis() == 0);
    sp5_fake_platform fake;
    sp5_fake_init(&fake);
    sp5_metrics *metrics =
        sp5_metrics_create(1, (sp5_metrics_capacity){131072, 2097152, 64}, &fake.clock);
    CHECK(metrics != NULL);
    sp5_metrics_view view;
    CHECK(sp5_metrics_snapshot(metrics, &view).status == SP5_OK);
    CHECK(view.allocated_bytes <= 83886080 && view.allocated_bytes >= 75497472);
    CHECK(sp5_metrics_enable(metrics, true, 0).status == SP5_OK);
    sp5_input_record event = input(1, 1, 0);
    event.flags = 0; /* failed/unconfirmed delivery is never a zero duration */
    CHECK(sp5_metrics_push_input(metrics, &event).status == SP5_OK);
    sp5_frame_record unavailable = frame(1, 1, 10);
    unavailable.flags &= ~(UINT64_C(1) << SP5_STAGE_ENCODE);
    CHECK(sp5_metrics_push_frame(metrics, &unavailable).status == SP5_OK);
    CHECK(sp5_metrics_worker_step(metrics, 1000000, NULL).status == SP5_OK);
    CHECK(sp5_metrics_snapshot(metrics, &view).status == SP5_OK);
    CHECK(view.failed_deliveries == 1);
    CHECK(!view.whole[SP5_STAGE_INPUT_DELIVERY].p99.value_us.present);
    CHECK(!view.whole[SP5_STAGE_ENCODE].p99.value_us.present);
    CHECK(view.whole[SP5_STAGE_CAPTURE].p99.value_us.present);
    sp5_metrics_destroy(metrics);
    CHECK(sp5_metrics_create(2, (sp5_metrics_capacity){SIZE_MAX, 1, 1}, &fake.clock) == NULL);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "percentiles") == 0) {
        return percentiles();
    }
    if (strcmp(argv[1], "overflow") == 0) {
        return overflow();
    }
    if (strcmp(argv[1], "counters_thresholds") == 0) {
        return counters_and_thresholds();
    }
    if (strcmp(argv[1], "rolling") == 0) {
        return rolling();
    }
    if (strcmp(argv[1], "retention") == 0) {
        return retention();
    }
    if (strcmp(argv[1], "toggles") == 0) {
        return toggles();
    }
    if (strcmp(argv[1], "queue_loss") == 0) {
        return queue_loss();
    }
    if (strcmp(argv[1], "off_state") == 0) {
        return off_state();
    }
    if (strcmp(argv[1], "nonblocking") == 0) {
        return nonblocking();
    }
    if (strcmp(argv[1], "limits_delivery") == 0) {
        return limits_and_delivery();
    }
    return 2;
}
