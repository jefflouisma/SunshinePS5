#include "internal.h"
#include <string.h>

void sp5_histogram_init(sp5_histogram *histogram)
{
    if (histogram != NULL) {
        memset(histogram, 0, sizeof(*histogram));
        histogram->validity = SP5_VALIDITY_VALID;
    }
}

sp5_result sp5_histogram_add(sp5_histogram *histogram, uint64_t value_us)
{
    if (histogram == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    uint64_t *bin =
        value_us < SP5_HISTOGRAM_BINS ? &histogram->bins[value_us] : &histogram->overflow;
    if (histogram->count == UINT64_MAX || *bin == UINT64_MAX) {
        histogram->validity = SP5_VALIDITY_INVALID;
        histogram->reason = SP5_REASON_OVERFLOW;
        return sp5_failure(SP5_REASON_OVERFLOW);
    }
    ++histogram->count;
    ++*bin;
    return sp5_success();
}

sp5_quantile sp5_histogram_percentile(const sp5_histogram *histogram, unsigned percentile)
{
    sp5_quantile result = {.reason = SP5_REASON_STAGE_UNAVAILABLE};
    if (histogram == NULL || percentile == 0 || percentile > 100) {
        result.reason = SP5_REASON_INVALID_ARGUMENT;
        return result;
    }
    result.population = histogram->count;
    if (histogram->validity != SP5_VALIDITY_VALID || histogram->missing != 0) {
        result.validity = SP5_VALIDITY_INVALID;
        result.reason =
            histogram->reason == SP5_REASON_NONE ? SP5_REASON_COVERAGE_GAP : histogram->reason;
        return result;
    }
    if (histogram->count == 0) {
        return result;
    }
    uint64_t rank =
        histogram->count / 100 * percentile + (histogram->count % 100 * percentile + 99) / 100;
    uint64_t cumulative = 0;
    for (size_t bin = 0; bin < SP5_HISTOGRAM_BINS; ++bin) {
        if (histogram->bins[bin] > histogram->count - cumulative) {
            result.validity = SP5_VALIDITY_INVALID;
            result.reason = SP5_REASON_OVERFLOW;
            return result;
        }
        cumulative += histogram->bins[bin];
        if (cumulative >= rank) {
            result.value_us = (sp5_optional_u64){true, bin};
            result.validity = SP5_VALIDITY_VALID;
            result.reason = SP5_REASON_NONE;
            return result;
        }
    }
    if (histogram->overflow != histogram->count - cumulative) {
        result.validity = SP5_VALIDITY_INVALID;
        result.reason = SP5_REASON_COVERAGE_GAP;
        return result;
    }
    result.lower_bound_us = (sp5_optional_u64){true, SP5_HISTOGRAM_BINS};
    result.reason = SP5_REASON_OVERFLOW;
    return result;
}

bool sp5_latency_below_frame_interval(uint64_t value_us, uint32_t fps_numerator,
                                      uint32_t fps_denominator)
{
    if (fps_numerator == 0 || fps_denominator == 0) {
        return false;
    }
    uint64_t numerator = UINT64_C(1000000) * fps_denominator;
    return value_us < numerator / fps_numerator ||
           (value_us == numerator / fps_numerator && numerator % fps_numerator != 0);
}

static void adjust(sp5_metrics *metrics, sp5_histogram *histogram, uint64_t value, bool valid,
                   bool failed, bool remove)
{
    if (!remove) {
        if (valid) {
            if (sp5_histogram_add(histogram, value).status != SP5_OK) {
                sp5_metrics_invalidate(metrics, SP5_REASON_OVERFLOW);
            }
        } else {
            if (!sp5_metrics_count(metrics, &histogram->missing, 1)) {
                histogram->validity = SP5_VALIDITY_INVALID;
                histogram->reason = SP5_REASON_OVERFLOW;
            }
        }
        if (failed && !sp5_metrics_count(metrics, &histogram->failed_deliveries, 1)) {
            histogram->validity = SP5_VALIDITY_INVALID;
            histogram->reason = SP5_REASON_OVERFLOW;
        }
        return;
    }
    uint64_t *bin = value < SP5_HISTOGRAM_BINS ? &histogram->bins[value] : &histogram->overflow;
    if ((valid && (histogram->count == 0 || *bin == 0)) || (!valid && histogram->missing == 0) ||
        (failed && histogram->failed_deliveries == 0)) {
        histogram->validity = SP5_VALIDITY_INVALID;
        histogram->reason = SP5_REASON_COVERAGE_GAP;
        sp5_metrics_invalidate(metrics, SP5_REASON_COVERAGE_GAP);
        return;
    }
    if (valid) {
        --histogram->count;
        --*bin;
    } else {
        --histogram->missing;
    }
    if (failed) {
        --histogram->failed_deliveries;
    }
}

void sp5_metrics_aggregate_frame(sp5_metrics *metrics, const sp5_frame_record *record, bool rolling,
                                 bool remove)
{
    sp5_histogram *set = rolling ? metrics->rolling : metrics->whole;
    if (!remove && (record->flags & SP5_RECORD_CLOCK_INVALID) != 0) {
        sp5_metrics_invalidate(metrics, SP5_REASON_CLOCK);
    } else if (!remove && (record->flags & SP5_FRAME_ALL_VALID) != SP5_FRAME_ALL_VALID) {
        sp5_metrics_invalidate(metrics, SP5_REASON_STAGE_UNAVAILABLE);
    }
    for (size_t i = 0; i < 5; ++i) {
        bool valid = (record->flags & (UINT64_C(1) << i)) != 0 &&
                     (record->flags & SP5_RECORD_CLOCK_INVALID) == 0;
        adjust(metrics, &set[i], record->duration_us[i], valid, false, remove);
        if (!valid && !remove && set[i].validity == SP5_VALIDITY_VALID) {
            bool clock_fault =
                (record->flags & SP5_RECORD_CLOCK_INVALID) != 0 ||
                (record->flags & (UINT64_C(1) << (SP5_RECORD_STAGE_CLOCK_SHIFT + i))) != 0;
            set[i].reason = clock_fault ? SP5_REASON_CLOCK : SP5_REASON_STAGE_UNAVAILABLE;
            sp5_metrics_invalidate(metrics, set[i].reason);
        }
    }
}

void sp5_metrics_aggregate_input(sp5_metrics *metrics, const sp5_input_record *record, bool rolling,
                                 bool remove)
{
    bool valid = (record->flags & (SP5_INPUT_VALID | SP5_INPUT_CONFIRMED)) ==
                     (SP5_INPUT_VALID | SP5_INPUT_CONFIRMED) &&
                 (record->flags & SP5_RECORD_CLOCK_INVALID) == 0;
    sp5_histogram *set = rolling ? metrics->rolling : metrics->whole;
    if (!remove && !valid) {
        sp5_metrics_invalidate(metrics, (record->flags & SP5_RECORD_CLOCK_INVALID) != 0
                                            ? SP5_REASON_CLOCK
                                            : SP5_REASON_DELIVERY_UNCONFIRMED);
    }
    adjust(metrics, &set[SP5_STAGE_INPUT_DELIVERY], record->duration_us, valid, !valid, remove);
    if (!valid && !remove && set[SP5_STAGE_INPUT_DELIVERY].validity == SP5_VALIDITY_VALID) {
        set[SP5_STAGE_INPUT_DELIVERY].reason =
            (record->flags & (SP5_RECORD_CLOCK_INVALID | SP5_INPUT_DURATION_CLOCK_INVALID)) != 0
                ? SP5_REASON_CLOCK
                : SP5_REASON_DELIVERY_UNCONFIRMED;
    }
}
