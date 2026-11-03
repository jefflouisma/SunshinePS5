#include "internal.h"
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(sp5_frame_record) == 64, "frame raw record budget");
_Static_assert(sizeof(sp5_input_record) == 32, "input raw record budget");
_Static_assert(sizeof(sp5_metrics) + (size_t)SP5_METRICS_QUEUE_CAPACITY * 96 < 8388608,
               "histograms, queues, coverage and snapshots fit 8 MiB");

static void queue_init(sp5_record_queue *queue, void *records)
{
    atomic_init(&queue->head, 0);
    atomic_init(&queue->tail, 0);
    atomic_init(&queue->lost, 0);
    atomic_init(&queue->overflow, false);
    queue->records = records;
}

sp5_metrics *sp5_metrics_create(uint64_t session_id, sp5_metrics_capacity capacity,
                                const sp5_clock_ops *clock)
{
    if (session_id == 0 || clock == NULL || clock->describe == NULL || clock->now == NULL ||
        capacity.frame_records == 0 || capacity.frame_records > SP5_METRICS_FRAME_CAPACITY ||
        capacity.input_records == 0 || capacity.input_records > SP5_METRICS_INPUT_CAPACITY ||
        capacity.queue_records == 0 || capacity.queue_records > SP5_METRICS_QUEUE_CAPACITY) {
        return NULL;
    }
    size_t bytes = sizeof(sp5_metrics) + capacity.frame_records * sizeof(sp5_frame_record) +
                   capacity.input_records * sizeof(sp5_input_record) +
                   capacity.queue_records * (sizeof(sp5_frame_record) + sizeof(sp5_input_record));
    if (bytes > SP5_METRICS_BYTES) {
        return NULL;
    }
    sp5_metrics *metrics = calloc(1, bytes);
    if (metrics == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&metrics->state_lock, NULL) != 0) {
        free(metrics);
        return NULL;
    }
    metrics->session_id = session_id;
    metrics->capacity = capacity;
    metrics->allocated_bytes = bytes;
    metrics->clock = *clock;
    sp5_result described = clock->describe(clock->context, &metrics->description);
    if (described.status != SP5_OK || metrics->description.validity != SP5_VALIDITY_VALID ||
        !metrics->description.monotonic || metrics->description.ticks_per_second == 0 ||
        metrics->description.native_resolution_ticks == 0) {
        metrics->coverage_reason = SP5_REASON_CLOCK;
    }
    atomic_init(&metrics->enabled, false);
    unsigned char *storage = (unsigned char *)(metrics + 1);
    metrics->frame_raw = (sp5_frame_record *)storage;
    storage += capacity.frame_records * sizeof(sp5_frame_record);
    metrics->input_raw = (sp5_input_record *)storage;
    storage += capacity.input_records * sizeof(sp5_input_record);
    queue_init(&metrics->frames, storage);
    storage += capacity.queue_records * sizeof(sp5_frame_record);
    queue_init(&metrics->inputs, storage);
    if (!atomic_is_lock_free(&metrics->enabled) || !atomic_is_lock_free(&metrics->frames.head) ||
        !atomic_is_lock_free(&metrics->frames.lost) ||
        !atomic_is_lock_free(&metrics->frames.overflow)) {
        sp5_metrics_destroy(metrics);
        return NULL;
    }
    for (size_t channel = 0; channel < SP5_STAGE_COUNT; ++channel) {
        metrics->whole[channel].validity = SP5_VALIDITY_VALID;
        metrics->rolling[channel].validity = SP5_VALIDITY_VALID;
    }
    metrics->context.synthetic = true;
    for (size_t i = 0; i < 9; ++i) {
        atomic_init(&metrics->continuous[i], 0);
    }
    atomic_init(&metrics->continuous_overflow, false);
    atomic_init(&metrics->continuous_sequence[0], 0);
    atomic_init(&metrics->continuous_sequence[1], 0);
    atomic_init(&metrics->exporting, false);
    sp5_timestamp started;
    sp5_duration elapsed;
    if (metrics->clock.now(metrics->clock.context, &started).status == SP5_OK) {
        sp5_timestamp origin = started;
        origin.ticks = 0;
        if (sp5_duration_between(&metrics->description, origin, started, &elapsed).status ==
            SP5_OK) {
            metrics->session_start_us = elapsed.value_us.value;
        } else {
            metrics->coverage_reason = SP5_REASON_CLOCK;
        }
    } else {
        metrics->coverage_reason = SP5_REASON_CLOCK;
    }
    metrics->transition_us = metrics->session_start_us;
    metrics->disabled_intervals = 1;
    metrics->intervals[0] =
        (sp5_metrics_interval){metrics->session_start_us, metrics->session_start_us, false};
    metrics->interval_count = 1;
    metrics->view.session_id = session_id;
    metrics->view.allocated_bytes = bytes;
    sp5_metrics_publish(metrics, metrics->session_start_us);
    return metrics;
}

void sp5_metrics_destroy(sp5_metrics *metrics)
{
    if (metrics != NULL) {
        if (pthread_mutex_destroy(&metrics->state_lock) != 0) {
            /* A busy owner must be quiesced before its allocation can be freed. */
            return;
        }
        free(metrics);
    }
}

static sp5_result push(sp5_metrics *metrics, sp5_record_queue *queue, const void *record,
                       size_t record_size)
{
    if (!atomic_load_explicit(&metrics->enabled, memory_order_acquire)) {
        return sp5_success();
    }
    size_t head = atomic_load_explicit(&queue->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&queue->tail, memory_order_acquire);
    /* Unsigned cursors may wrap: distance remains bounded by queue capacity. */
    size_t cycle = metrics->capacity.queue_records * 2;
    if ((head + cycle - tail) % cycle >= metrics->capacity.queue_records) {
        uint64_t lost = atomic_load_explicit(&queue->lost, memory_order_relaxed);
        if (lost == UINT64_MAX) {
            atomic_store_explicit(&queue->overflow, true, memory_order_release);
        } else {
            atomic_store_explicit(&queue->lost, lost + 1, memory_order_release);
        }
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    memcpy((unsigned char *)queue->records + (head % metrics->capacity.queue_records) * record_size,
           record, record_size);
    atomic_store_explicit(&queue->head, (head + 1) % cycle, memory_order_release);
    return sp5_success();
}

sp5_result sp5_metrics_push_frame(sp5_metrics *metrics, const sp5_frame_record *record)
{
    if (metrics == NULL || record == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    return push(metrics, &metrics->frames, record, sizeof(*record));
}

sp5_result sp5_metrics_push_input(sp5_metrics *metrics, const sp5_input_record *record)
{
    if (metrics == NULL || record == NULL || record->controller_id >= SP5_MAX_CONTROLLERS) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    return push(metrics, &metrics->inputs, record, sizeof(*record));
}

sp5_result sp5_metrics_stamp(sp5_metrics *metrics, sp5_timestamp *timestamp)
{
    if (timestamp == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *timestamp = (sp5_timestamp){.reason = SP5_REASON_METRICS_DISABLED};
    if (metrics == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (!atomic_load_explicit(&metrics->enabled, memory_order_acquire)) {
        return sp5_failure(SP5_REASON_METRICS_DISABLED);
    }
    sp5_result result = metrics->clock.now(metrics->clock.context, timestamp);
    sp5_duration check;
    if (result.status != SP5_OK ||
        sp5_duration_between(&metrics->description, *timestamp, *timestamp, &check).status !=
            SP5_OK) {
        *timestamp = (sp5_timestamp){.validity = SP5_VALIDITY_INVALID, .reason = SP5_REASON_CLOCK};
        return sp5_failure(SP5_REASON_CLOCK);
    }
    return sp5_success();
}

sp5_result sp5_metrics_protocol_header(sp5_metrics *metrics, uint64_t frame_id,
                                       sp5_timestamp capture, bool repeated,
                                       sp5_protocol_latency_evidence *evidence)
{
    if (metrics == NULL || evidence == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *evidence = (sp5_protocol_latency_evidence){.session_id = metrics->session_id,
                                                .frame_id = frame_id,
                                                .capture_timestamp = capture,
                                                .repeated = repeated,
                                                .reason = SP5_REASON_STAGE_UNAVAILABLE};
    if (repeated || capture.validity != SP5_VALIDITY_VALID) {
        return sp5_success();
    }
    sp5_duration duration;
    sp5_result result = metrics->clock.now(metrics->clock.context, &evidence->header_measurement);
    if (result.status != SP5_OK || sp5_duration_between(&metrics->description, capture,
                                                        evidence->header_measurement, &duration)
                                           .status != SP5_OK) {
        evidence->validity = SP5_VALIDITY_INVALID;
        evidence->reason = SP5_REASON_CLOCK;
        return sp5_failure(SP5_REASON_CLOCK);
    }
    /* protocol-notes.md §6: nearest 0.1 ms, saturation; never final-send timing. */
    uint64_t us = duration.value_us.value;
    uint64_t wire = us / 100 + (us % 100 >= 50 ? 1 : 0);
    evidence->wire_tenths_ms = (uint16_t)(wire > UINT16_MAX ? UINT16_MAX : wire);
    evidence->validity = SP5_VALIDITY_VALID;
    evidence->reason = SP5_REASON_NONE;
    return sp5_success();
}

static sp5_result observation_time(sp5_metrics *metrics, sp5_timestamp timestamp, uint64_t *us)
{
    sp5_timestamp origin = timestamp;
    origin.ticks = 0;
    sp5_duration duration;
    sp5_result result = sp5_duration_between(&metrics->description, origin, timestamp, &duration);
    if (result.status == SP5_OK) {
        *us = duration.value_us.value;
    }
    return result;
}

sp5_result sp5_metrics_record_frame(sp5_metrics *metrics, const sp5_frame_evidence *evidence)
{
    if (metrics == NULL || evidence == NULL ||
        (unsigned)evidence->outcome > SP5_FRAME_HOST_DROPPED) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (evidence->session_id != metrics->session_id) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    if (!atomic_load_explicit(&metrics->enabled, memory_order_acquire)) {
        return sp5_success();
    }
    sp5_frame_record record = {.frame_id = evidence->frame_id,
                               .flags = ((uint64_t)evidence->outcome << SP5_FRAME_OUTCOME_SHIFT) |
                                        ((uint64_t)evidence->reason << SP5_RECORD_REASON_SHIFT)};
    if (observation_time(metrics, evidence->stages[SP5_STAGE_FULL_PIPELINE].end,
                         &record.observed_us)
            .status != SP5_OK) {
        record.flags |= SP5_RECORD_CLOCK_INVALID;
    }
    for (size_t i = 0; i < 5; ++i) {
        sp5_duration duration;
        const sp5_stage_span *span = &evidence->stages[i];
        if (span->validity == SP5_VALIDITY_VALID) {
            if (sp5_duration_between(&metrics->description, span->start, span->end, &duration)
                    .status == SP5_OK) {
                record.flags |= UINT64_C(1) << i;
                record.duration_us[i] = duration.value_us.value;
            } else {
                record.flags |= UINT64_C(1) << (SP5_RECORD_STAGE_CLOCK_SHIFT + i);
            }
        }
    }
    return sp5_metrics_push_frame(metrics, &record);
}

sp5_result sp5_metrics_record_input(sp5_metrics *metrics, const sp5_input_evidence *evidence,
                                    bool authenticated)
{
    if (metrics == NULL || evidence == NULL || evidence->controller_id >= SP5_MAX_CONTROLLERS) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (!authenticated || evidence->session_id != metrics->session_id) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    if (!atomic_load_explicit(&metrics->enabled, memory_order_acquire)) {
        return sp5_success();
    }
    sp5_input_record record = {.event_id = evidence->event_id,
                               .controller_id = evidence->controller_id,
                               .flags = SP5_INPUT_AUTHENTICATED};
    if (observation_time(metrics, evidence->host_received, &record.observed_us).status != SP5_OK) {
        record.flags |= SP5_RECORD_CLOCK_INVALID;
    }
    if (evidence->delivery == SP5_INPUT_DELIVERY_CONFIRMED) {
        record.flags |= SP5_INPUT_CONFIRMED;
        sp5_duration duration;
        if (evidence->delivery_boundary_ref != NULL && evidence->delivery_boundary_ref[0] != '\0' &&
            sp5_duration_between(&metrics->description, evidence->host_received,
                                 evidence->console_delivered, &duration)
                    .status == SP5_OK) {
            record.flags |= SP5_INPUT_VALID;
            record.duration_us = duration.value_us.value;
        } else if (evidence->delivery_boundary_ref != NULL &&
                   evidence->delivery_boundary_ref[0] != '\0') {
            record.flags |= SP5_INPUT_DURATION_CLOCK_INVALID;
        }
    } else if (evidence->delivery == SP5_INPUT_DELIVERY_FAILED) {
        record.flags |= SP5_INPUT_FAILED;
    }
    return sp5_metrics_push_input(metrics, &record);
}
