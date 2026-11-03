#include "internal.h"
#include <string.h>

static sp5_distribution distribution(const sp5_histogram *histogram)
{
    return (sp5_distribution){sp5_histogram_percentile(histogram, 50),
                              sp5_histogram_percentile(histogram, 95),
                              sp5_histogram_percentile(histogram, 99)};
}

static sp5_metrics_population population(sp5_metrics *metrics, const sp5_histogram *histogram)
{
    uint64_t seen = histogram->count;
    if (!sp5_metrics_count(metrics, &seen, histogram->missing)) {
        seen = UINT64_MAX;
    }
    return (sp5_metrics_population){seen, histogram->count - histogram->overflow,
                                    histogram->overflow, histogram->missing,
                                    histogram->failed_deliveries};
}

/* Exact checked floor(value * multiplier / interval), with bounded 30-step
 * fractional division. Splitting the integer quotient avoids wide extensions. */
typedef struct {
    uint64_t value;
    uint32_t multiplier;
    uint64_t interval;
} rate_input;

static sp5_optional_u64 rate(rate_input input)
{
    uint64_t value = input.value;
    uint32_t multiplier = input.multiplier;
    uint64_t interval = input.interval;
    if (interval == 0) {
        return (sp5_optional_u64){0};
    }
    uint64_t integer = value / interval;
    uint64_t remainder = value % interval;
    uint64_t fractional = 0;
    uint64_t rest = 0;
    for (uint32_t bit = UINT32_C(1) << 29; bit != 0; bit >>= 1) {
        fractional *= 2;
        if (rest >= interval - rest) {
            rest -= interval - rest;
            ++fractional;
        } else {
            rest += rest;
        }
        if ((multiplier & bit) != 0) {
            if (rest >= interval - remainder) {
                rest -= interval - remainder;
                ++fractional;
            } else {
                rest += remainder;
            }
        }
    }
    if (integer > (UINT64_MAX - fractional) / multiplier) {
        return (sp5_optional_u64){0};
    }
    return (sp5_optional_u64){true, integer * multiplier + fractional};
}

void sp5_metrics_publish(sp5_metrics *metrics, uint64_t now)
{
    sp5_metrics_view *view = &metrics->view;
    view->session_id = metrics->session_id;
    view->published_us = now;
    view->allocated_bytes = metrics->allocated_bytes;
    view->clock = metrics->description;
    view->metrics_enabled = atomic_load_explicit(&metrics->enabled, memory_order_acquire);
    view->context = metrics->context;
    for (size_t i = 0; i < SP5_STAGE_COUNT; ++i) {
        view->whole_population[i] = population(metrics, &metrics->whole[i]);
        view->rolling_population[i] = population(metrics, &metrics->rolling[i]);
        view->whole[i] = distribution(&metrics->whole[i]);
        view->rolling[i] = distribution(&metrics->rolling[i]);
    }
    view->retained_frames = metrics->frame_ring.count;
    view->retained_inputs = metrics->input_ring.count;
    view->first_frame_us = (sp5_optional_u64){0};
    view->last_frame_us = (sp5_optional_u64){0};
    view->first_input_us = (sp5_optional_u64){0};
    view->last_input_us = (sp5_optional_u64){0};
    if (view->retained_frames > 0) {
        view->first_frame_us =
            (sp5_optional_u64){true, metrics->frame_raw[metrics->frame_ring.first].observed_us};
        view->last_frame_us = metrics->frame_ring.latest;
    }
    if (view->retained_inputs > 0) {
        view->first_input_us =
            (sp5_optional_u64){true, metrics->input_raw[metrics->input_ring.first].observed_us};
        view->last_input_us = metrics->input_ring.latest;
    }
    view->lost_frames = atomic_load_explicit(&metrics->frames.lost, memory_order_acquire);
    view->lost_inputs = atomic_load_explicit(&metrics->inputs.lost, memory_order_acquire);
    view->failed_deliveries = metrics->failed_deliveries;
    view->enabled_intervals = metrics->enabled_intervals;
    view->disabled_intervals = metrics->disabled_intervals;
    view->interval_count = metrics->interval_count;
    memcpy(view->intervals, metrics->intervals, metrics->interval_count * sizeof(*view->intervals));
    if (metrics->interval_count < SP5_METRICS_INTERVAL_CAPACITY ||
        metrics->coverage_reason != SP5_REASON_COVERAGE_GAP) {
        view->intervals[view->interval_count - 1].end_us = now;
    }
    sp5_metrics_continuous *continuous = &view->continuous;
    uint64_t media_sequence =
        atomic_load_explicit(&metrics->continuous_sequence[0], memory_order_seq_cst);
    uint64_t input_sequence =
        atomic_load_explicit(&metrics->continuous_sequence[1], memory_order_seq_cst);
    for (size_t i = 0; i < 5; ++i) {
        continuous->frame_outcomes[i] =
            atomic_load_explicit(&metrics->continuous[i], memory_order_seq_cst);
    }
    continuous->encoded_bytes = atomic_load_explicit(&metrics->continuous[5], memory_order_seq_cst);
    continuous->network_bytes = atomic_load_explicit(&metrics->continuous[6], memory_order_seq_cst);
    continuous->client_lost_packets =
        atomic_load_explicit(&metrics->continuous[7], memory_order_seq_cst);
    continuous->client_recovery_requests =
        atomic_load_explicit(&metrics->continuous[8], memory_order_seq_cst);
    continuous->start_us = metrics->session_start_us;
    continuous->end_us = now;
    uint64_t interval = now - metrics->session_start_us;
    continuous->encoded_bitrate_bps =
        rate((rate_input){continuous->encoded_bytes, 8000000, interval});
    continuous->network_throughput_bps =
        rate((rate_input){continuous->network_bytes, 8000000, interval});
    continuous->frame_rate_millihz = rate((rate_input){
        continuous->frame_outcomes[SP5_FRAME_NETWORK_SUBMITTED], 1000000000, interval});
    continuous->validity = SP5_VALIDITY_VALID;
    continuous->reason = SP5_REASON_NONE;
    if ((media_sequence & 1U) != 0 || (input_sequence & 1U) != 0 ||
        media_sequence !=
            atomic_load_explicit(&metrics->continuous_sequence[0], memory_order_seq_cst) ||
        input_sequence !=
            atomic_load_explicit(&metrics->continuous_sequence[1], memory_order_seq_cst)) {
        continuous->validity = SP5_VALIDITY_UNAVAILABLE;
        continuous->reason = SP5_REASON_WOULD_BLOCK;
    }
    if (atomic_load_explicit(&metrics->continuous_overflow, memory_order_seq_cst)) {
        continuous->validity = SP5_VALIDITY_INVALID;
        continuous->reason = SP5_REASON_OVERFLOW;
        sp5_metrics_invalidate(metrics, SP5_REASON_OVERFLOW);
    }
    if (continuous->validity != SP5_VALIDITY_VALID || interval == 0) {
        continuous->encoded_bitrate_bps.present = false;
        continuous->network_throughput_bps.present = false;
        continuous->frame_rate_millihz.present = false;
    } else if (!continuous->encoded_bitrate_bps.present ||
               !continuous->network_throughput_bps.present ||
               !continuous->frame_rate_millihz.present) {
        continuous->validity = SP5_VALIDITY_INVALID;
        continuous->reason = SP5_REASON_OVERFLOW;
    }
    view->coverage_reason = metrics->coverage_reason;
    view->coverage_validity =
        metrics->coverage_reason == SP5_REASON_NONE ? SP5_VALIDITY_VALID : SP5_VALIDITY_INVALID;
}

static void mark_loss(sp5_metrics *metrics, sp5_record_queue *queue, uint64_t *applied,
                      size_t first, size_t end, uint64_t now)
{
    uint64_t lost = atomic_load_explicit(&queue->lost, memory_order_acquire);
    bool overflow = atomic_load_explicit(&queue->overflow, memory_order_acquire);
    if (lost == *applied && !overflow) {
        return;
    }
    sp5_metrics_invalidate(metrics, overflow ? SP5_REASON_OVERFLOW : SP5_REASON_COVERAGE_GAP);
    for (size_t i = first; i < end; ++i) {
        if (!sp5_metrics_count(metrics, &metrics->whole[i].missing, lost - *applied) || overflow) {
            metrics->whole[i].validity = SP5_VALIDITY_INVALID;
            metrics->whole[i].reason = SP5_REASON_OVERFLOW;
        }
        if (!sp5_metrics_count(metrics, &metrics->rolling[i].missing, lost - *applied) ||
            !sp5_metrics_count(metrics, &metrics->rolling_gap_missing[i], lost - *applied)) {
            metrics->rolling_permanent[i] = true;
        }
        metrics->rolling[i].validity = SP5_VALIDITY_INVALID;
        metrics->rolling[i].reason = overflow ? SP5_REASON_OVERFLOW : SP5_REASON_COVERAGE_GAP;
        if (overflow || now > UINT64_MAX - SP5_ROLLING_US) {
            metrics->rolling_permanent[i] = true;
        } else {
            metrics->rolling_invalid_until[i] = now + SP5_ROLLING_US;
        }
    }
    *applied = lost;
}

static bool valid_time(sp5_metrics *metrics, sp5_record_ring *ring, uint64_t at, uint64_t now,
                       size_t first, size_t end)
{
    if (at <= now && (!ring->latest.present || at >= ring->latest.value)) {
        return true;
    }
    sp5_metrics_invalidate(metrics, SP5_REASON_CLOCK);
    for (size_t i = first; i < end; ++i) {
        metrics->whole[i].validity = SP5_VALIDITY_INVALID;
        metrics->whole[i].reason = SP5_REASON_CLOCK;
        metrics->rolling[i].validity = SP5_VALIDITY_INVALID;
        metrics->rolling[i].reason = SP5_REASON_CLOCK;
        metrics->rolling_permanent[i] = true;
    }
    return false;
}

sp5_result sp5_metrics_worker_step(sp5_metrics *metrics, uint64_t now_us,
                                   const sp5_metrics_observer *observer)
{
    if (metrics == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (pthread_mutex_trylock(&metrics->state_lock) != 0) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    if ((metrics->worker_started && now_us < metrics->worker_us) ||
        now_us < metrics->transition_us) {
        sp5_metrics_invalidate(metrics, SP5_REASON_CLOCK);
        for (size_t i = 0; i < SP5_STAGE_COUNT; ++i) {
            metrics->whole[i].validity = SP5_VALIDITY_INVALID;
            metrics->whole[i].reason = SP5_REASON_CLOCK;
            metrics->rolling[i].validity = SP5_VALIDITY_INVALID;
            metrics->rolling[i].reason = SP5_REASON_CLOCK;
            metrics->rolling_permanent[i] = true;
        }
        sp5_metrics_publish(metrics, metrics->worker_us);
        if (pthread_mutex_unlock(&metrics->state_lock) != 0) {
            return sp5_failure(SP5_REASON_PLATFORM);
        }
        return sp5_failure(SP5_REASON_CLOCK);
    }
    metrics->worker_us = now_us;
    metrics->worker_started = true;
    sp5_metrics_expire(metrics, now_us);
    /* Fixed initial head bounds this pass even when producers remain active. */
    size_t tail = atomic_load_explicit(&metrics->frames.tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&metrics->frames.head, memory_order_acquire);
    while (tail != head) {
        sp5_frame_record record =
            ((sp5_frame_record *)metrics->frames.records)[tail % metrics->capacity.queue_records];
        tail = (tail + 1) % (metrics->capacity.queue_records * 2);
        atomic_store_explicit(&metrics->frames.tail, tail, memory_order_release);
        if (!valid_time(metrics, &metrics->frame_ring, record.observed_us, now_us, 0, 5)) {
            record.flags |= SP5_RECORD_CLOCK_INVALID;
            sp5_metrics_aggregate_frame(metrics, &record, false, false);
            continue;
        }
        sp5_metrics_aggregate_frame(metrics, &record, false, false);
        sp5_metrics_aggregate_frame(metrics, &record, true, false);
        sp5_metrics_retain_frame(metrics, &record, now_us);
        sp5_metrics_expire(metrics, now_us);
    }
    tail = atomic_load_explicit(&metrics->inputs.tail, memory_order_relaxed);
    head = atomic_load_explicit(&metrics->inputs.head, memory_order_acquire);
    while (tail != head) {
        sp5_input_record record =
            ((sp5_input_record *)metrics->inputs.records)[tail % metrics->capacity.queue_records];
        tail = (tail + 1) % (metrics->capacity.queue_records * 2);
        atomic_store_explicit(&metrics->inputs.tail, tail, memory_order_release);
        bool delivered = (record.flags & (SP5_INPUT_VALID | SP5_INPUT_CONFIRMED)) ==
                             (SP5_INPUT_VALID | SP5_INPUT_CONFIRMED) &&
                         (record.flags & SP5_RECORD_CLOCK_INVALID) == 0;
        if (!delivered && !sp5_metrics_count(metrics, &metrics->failed_deliveries, 1)) {
            metrics->whole[SP5_STAGE_INPUT_DELIVERY].validity = SP5_VALIDITY_INVALID;
            metrics->whole[SP5_STAGE_INPUT_DELIVERY].reason = SP5_REASON_OVERFLOW;
        }
        if (!valid_time(metrics, &metrics->input_ring, record.observed_us, now_us,
                        SP5_STAGE_INPUT_DELIVERY, SP5_STAGE_COUNT)) {
            record.flags |= SP5_RECORD_CLOCK_INVALID;
            sp5_metrics_aggregate_input(metrics, &record, false, false);
            continue;
        }
        sp5_metrics_aggregate_input(metrics, &record, false, false);
        sp5_metrics_aggregate_input(metrics, &record, true, false);
        sp5_metrics_retain_input(metrics, &record, now_us);
        sp5_metrics_expire(metrics, now_us);
    }
    mark_loss(metrics, &metrics->frames, &metrics->frame_losses_applied, 0, 5, now_us);
    mark_loss(metrics, &metrics->inputs, &metrics->input_losses_applied, SP5_STAGE_INPUT_DELIVERY,
              SP5_STAGE_COUNT, now_us);
    sp5_metrics_publish(metrics, now_us);
    sp5_metrics_view snapshot = metrics->view;
    if (pthread_mutex_unlock(&metrics->state_lock) != 0) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    /* The callback sees an immutable copy and owns no engine lock. */
    if (observer != NULL && observer->published != NULL) {
        observer->published(observer->context, &snapshot);
    }
    return sp5_success();
}

sp5_result sp5_metrics_snapshot(const sp5_metrics *metrics, sp5_metrics_view *view)
{
    if (metrics == NULL || view == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    /* The lock protects mutable state behind the otherwise read-only handle.
     * Readers wait for a bounded worker pass (no holder waits on anything else)
     * instead of failing the view; producers and the worker never wait here. */
    sp5_metrics *state = (sp5_metrics *)metrics;
    if (pthread_mutex_lock(&state->state_lock) != 0) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    *view = metrics->view;
    view->metrics_enabled = atomic_load_explicit(&metrics->enabled, memory_order_acquire);
    if (pthread_mutex_unlock(&state->state_lock) != 0) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    return sp5_success();
}
