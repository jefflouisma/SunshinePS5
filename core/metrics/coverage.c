#include "internal.h"
#include <string.h>

void sp5_metrics_invalidate(sp5_metrics *metrics, sp5_reason reason)
{
    if (metrics->coverage_reason == SP5_REASON_NONE || reason == SP5_REASON_OVERFLOW) {
        metrics->coverage_reason = reason;
    }
}

bool sp5_metrics_count(sp5_metrics *metrics, uint64_t *counter, uint64_t increment)
{
    if (*counter > UINT64_MAX - increment) {
        sp5_metrics_invalidate(metrics, SP5_REASON_OVERFLOW);
        return false;
    }
    *counter += increment;
    return true;
}

sp5_result sp5_metrics_enable(sp5_metrics *metrics, bool enabled, uint64_t now_us)
{
    if (metrics == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (pthread_mutex_trylock(&metrics->state_lock) != 0) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    sp5_result result = sp5_success();
    if (now_us < metrics->transition_us ||
        (metrics->worker_started && now_us < metrics->worker_us)) {
        sp5_metrics_invalidate(metrics, SP5_REASON_CLOCK);
        result = sp5_failure(SP5_REASON_CLOCK);
    } else if (enabled != atomic_load_explicit(&metrics->enabled, memory_order_relaxed)) {
        uint64_t *count = enabled ? &metrics->enabled_intervals : &metrics->disabled_intervals;
        if (!sp5_metrics_count(metrics, count, 1)) {
            result = sp5_failure(SP5_REASON_OVERFLOW);
        } else {
            metrics->intervals[metrics->interval_count - 1].end_us = now_us;
            if (metrics->interval_count == SP5_METRICS_INTERVAL_CAPACITY) {
                sp5_metrics_invalidate(metrics, SP5_REASON_COVERAGE_GAP);
                result = sp5_failure(SP5_REASON_CAPACITY);
            } else {
                metrics->intervals[metrics->interval_count++] =
                    (sp5_metrics_interval){now_us, now_us, enabled};
            }
            metrics->transition_us = now_us;
            atomic_store_explicit(&metrics->enabled, enabled, memory_order_release);
        }
    }
    if (pthread_mutex_unlock(&metrics->state_lock) != 0) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    return result;
}

static bool in_window(uint64_t timestamp, uint64_t now)
{
    return timestamp <= now && now - timestamp < SP5_ROLLING_US;
}

static void lost_expiry(sp5_metrics *metrics, size_t channel, uint64_t timestamp)
{
    if (timestamp > UINT64_MAX - SP5_ROLLING_US) {
        metrics->rolling_permanent[channel] = true;
    } else if (metrics->rolling_invalid_until[channel] < timestamp + SP5_ROLLING_US) {
        metrics->rolling_invalid_until[channel] = timestamp + SP5_ROLLING_US;
    }
    if (!sp5_metrics_count(metrics, &metrics->rolling[channel].missing, 1) ||
        !sp5_metrics_count(metrics, &metrics->rolling_gap_missing[channel], 1)) {
        metrics->rolling_permanent[channel] = true;
    }
    metrics->rolling[channel].validity = SP5_VALIDITY_INVALID;
    metrics->rolling[channel].reason = SP5_REASON_COVERAGE_GAP;
}

static void evict_frame(sp5_metrics *metrics, uint64_t now)
{
    sp5_record_ring *ring = &metrics->frame_ring;
    const sp5_frame_record *old = &metrics->frame_raw[ring->first];
    if (ring->rolling_expired == 0) {
        sp5_metrics_aggregate_frame(metrics, old, true, true);
        if (in_window(old->observed_us, now)) {
            for (size_t i = 0; i < 5; ++i) {
                lost_expiry(metrics, i, old->observed_us);
            }
        }
    } else {
        --ring->rolling_expired;
    }
    ring->first = (ring->first + 1) % metrics->capacity.frame_records;
    --ring->count;
}

static void evict_input(sp5_metrics *metrics, uint64_t now)
{
    sp5_record_ring *ring = &metrics->input_ring;
    const sp5_input_record *old = &metrics->input_raw[ring->first];
    if (ring->rolling_expired == 0) {
        sp5_metrics_aggregate_input(metrics, old, true, true);
        if (in_window(old->observed_us, now)) {
            lost_expiry(metrics, SP5_STAGE_INPUT_DELIVERY, old->observed_us);
        }
    } else {
        --ring->rolling_expired;
    }
    ring->first = (ring->first + 1) % metrics->capacity.input_records;
    --ring->count;
}

void sp5_metrics_retain_frame(sp5_metrics *metrics, const sp5_frame_record *record, uint64_t now)
{
    sp5_record_ring *ring = &metrics->frame_ring;
    if (ring->count == metrics->capacity.frame_records) {
        evict_frame(metrics, now);
    }
    size_t at = (ring->first + ring->count) % metrics->capacity.frame_records;
    metrics->frame_raw[at] = *record;
    ++ring->count;
    if (sp5_metrics_count(metrics, &ring->inserted, 1)) {
        ring->latest = (sp5_optional_u64){true, record->observed_us};
    }
}

void sp5_metrics_retain_input(sp5_metrics *metrics, const sp5_input_record *record, uint64_t now)
{
    sp5_record_ring *ring = &metrics->input_ring;
    if (ring->count == metrics->capacity.input_records) {
        evict_input(metrics, now);
    }
    size_t at = (ring->first + ring->count) % metrics->capacity.input_records;
    metrics->input_raw[at] = *record;
    ++ring->count;
    if (sp5_metrics_count(metrics, &ring->inserted, 1)) {
        ring->latest = (sp5_optional_u64){true, record->observed_us};
    }
}

void sp5_metrics_expire(sp5_metrics *metrics, uint64_t now_us)
{
    sp5_record_ring *frames = &metrics->frame_ring;
    while (frames->rolling_expired < frames->count) {
        const sp5_frame_record *record =
            &metrics->frame_raw[(frames->first + frames->rolling_expired) %
                                metrics->capacity.frame_records];
        if (record->observed_us > now_us || now_us - record->observed_us < SP5_ROLLING_US) {
            break;
        }
        sp5_metrics_aggregate_frame(metrics, record, true, true);
        ++frames->rolling_expired;
    }
    while (frames->count > 0) {
        uint64_t at = metrics->frame_raw[frames->first].observed_us;
        if (at > now_us || now_us - at <= SP5_RETENTION_US) {
            break;
        }
        evict_frame(metrics, now_us);
    }
    sp5_record_ring *inputs = &metrics->input_ring;
    while (inputs->rolling_expired < inputs->count) {
        const sp5_input_record *record =
            &metrics->input_raw[(inputs->first + inputs->rolling_expired) %
                                metrics->capacity.input_records];
        if (record->observed_us > now_us || now_us - record->observed_us < SP5_ROLLING_US) {
            break;
        }
        sp5_metrics_aggregate_input(metrics, record, true, true);
        ++inputs->rolling_expired;
    }
    while (inputs->count > 0) {
        uint64_t at = metrics->input_raw[inputs->first].observed_us;
        if (at > now_us || now_us - at <= SP5_RETENTION_US) {
            break;
        }
        evict_input(metrics, now_us);
    }
    for (size_t i = 0; i < SP5_STAGE_COUNT; ++i) {
        if (!metrics->rolling_permanent[i] && metrics->rolling_invalid_until[i] != 0 &&
            now_us >= metrics->rolling_invalid_until[i]) {
            if (metrics->rolling[i].missing < metrics->rolling_gap_missing[i]) {
                metrics->rolling_permanent[i] = true;
                sp5_metrics_invalidate(metrics, SP5_REASON_OVERFLOW);
                continue;
            }
            metrics->rolling[i].missing -= metrics->rolling_gap_missing[i];
            metrics->rolling_gap_missing[i] = 0;
            metrics->rolling[i].validity = SP5_VALIDITY_VALID;
            metrics->rolling[i].reason = SP5_REASON_NONE;
            metrics->rolling_invalid_until[i] = 0;
        }
    }
}

sp5_result sp5_metrics_set_context(sp5_metrics *metrics, const sp5_metrics_context *context)
{
    if (metrics == NULL || context == NULL ||
        memchr(context->host_version, 0, sizeof(context->host_version)) == NULL ||
        memchr(context->firmware_version, 0, sizeof(context->firmware_version)) == NULL ||
        memchr(context->client_name, 0, sizeof(context->client_name)) == NULL ||
        memchr(context->codec, 0, sizeof(context->codec)) == NULL ||
        memchr(context->delivery_boundary, 0, sizeof(context->delivery_boundary)) == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (pthread_mutex_trylock(&metrics->state_lock) != 0) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    sp5_result result = sp5_success();
    if (metrics->worker_started || atomic_load_explicit(&metrics->enabled, memory_order_acquire)) {
        result = sp5_failure(SP5_REASON_OWNERSHIP);
    } else {
        metrics->context = *context;
        metrics->context.mode.codec = NULL;
    }
    if (pthread_mutex_unlock(&metrics->state_lock) != 0) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    return result;
}

static bool add_continuous(sp5_metrics *metrics, size_t index, uint64_t increment)
{
    /* Each counter has one producer, so checked load/store needs no retry loop. */
    uint64_t value = atomic_load_explicit(&metrics->continuous[index], memory_order_seq_cst);
    if (value > UINT64_MAX - increment) {
        atomic_store_explicit(&metrics->continuous_overflow, true, memory_order_seq_cst);
        return false;
    }
    atomic_store_explicit(&metrics->continuous[index], value + increment, memory_order_seq_cst);
    return true;
}

static bool begin_continuous(sp5_metrics *metrics, size_t owner, uint64_t *generation)
{
    *generation = atomic_load_explicit(&metrics->continuous_sequence[owner], memory_order_seq_cst);
    if (*generation > UINT64_MAX - 2) {
        atomic_store_explicit(&metrics->continuous_overflow, true, memory_order_release);
        return false;
    }
    atomic_store_explicit(&metrics->continuous_sequence[owner], *generation + 1,
                          memory_order_seq_cst);
    return true;
}

sp5_result sp5_metrics_count_stream(sp5_metrics *metrics, sp5_metrics_stream_event event)
{
    if (metrics == NULL || (unsigned)event.outcome > SP5_FRAME_HOST_DROPPED) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    uint64_t generation;
    if (!begin_continuous(metrics, 0, &generation)) {
        return sp5_failure(SP5_REASON_OVERFLOW);
    }
    bool outcome_ok = add_continuous(metrics, (size_t)event.outcome, 1);
    bool encoded_ok = add_continuous(metrics, 5, event.encoded_bytes);
    bool network_ok = add_continuous(metrics, 6, event.network_bytes);
    atomic_store_explicit(&metrics->continuous_sequence[0], generation + 2, memory_order_seq_cst);
    return outcome_ok && encoded_ok && network_ok ? sp5_success()
                                                  : sp5_failure(SP5_REASON_OVERFLOW);
}

sp5_result sp5_metrics_count_client(sp5_metrics *metrics, uint64_t lost_packets,
                                    uint64_t recovery_requests)
{
    if (metrics == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    uint64_t generation;
    if (!begin_continuous(metrics, 1, &generation)) {
        return sp5_failure(SP5_REASON_OVERFLOW);
    }
    bool lost_ok = add_continuous(metrics, 7, lost_packets);
    bool recovered_ok = add_continuous(metrics, 8, recovery_requests);
    atomic_store_explicit(&metrics->continuous_sequence[1], generation + 2, memory_order_seq_cst);
    return lost_ok && recovered_ok ? sp5_success() : sp5_failure(SP5_REASON_OVERFLOW);
}
