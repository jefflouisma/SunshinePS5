#include "runtime.h"

#include <string.h>

sp5_result sp5_success(void)
{
    return (sp5_result){SP5_OK, SP5_REASON_NONE};
}

sp5_result sp5_failure(sp5_reason reason)
{
    return (sp5_result){SP5_ERROR,
                        reason == SP5_REASON_NONE ? SP5_REASON_INVALID_ARGUMENT : reason};
}

const char *sp5_reason_name(sp5_reason reason)
{
    switch (reason) {
    case SP5_REASON_NONE:
        return "none";
    case SP5_REASON_UNSUPPORTED:
        return "unsupported";
    case SP5_REASON_UNVERIFIED:
        return "unverified";
    case SP5_REASON_INVALID_ARGUMENT:
        return "invalid_argument";
    case SP5_REASON_CAPACITY:
        return "capacity";
    case SP5_REASON_WOULD_BLOCK:
        return "would_block";
    case SP5_REASON_TIMEOUT:
        return "timeout";
    case SP5_REASON_CLOCK:
        return "clock";
    case SP5_REASON_OVERFLOW:
        return "overflow";
    case SP5_REASON_COVERAGE_GAP:
        return "coverage_gap";
    case SP5_REASON_METRICS_DISABLED:
        return "metrics_disabled";
    case SP5_REASON_STAGE_UNAVAILABLE:
        return "stage_unavailable";
    case SP5_REASON_PROTECTED:
        return "protected";
    case SP5_REASON_PROTECTION_UNKNOWN:
        return "protection_unknown";
    case SP5_REASON_SOURCE_CHANGED:
        return "source_changed";
    case SP5_REASON_CONFLICT:
        return "conflict";
    case SP5_REASON_DELIVERY_UNCONFIRMED:
        return "delivery_unconfirmed";
    case SP5_REASON_PLATFORM:
        return "platform";
    case SP5_REASON_OWNERSHIP:
        return "ownership";
    }
    return "unknown";
}

static bool buffer_valid(const sp5_buffer *buffer)
{
    return buffer != NULL && (buffer->data != NULL || buffer->capacity == 0) &&
           buffer->size <= buffer->capacity;
}

sp5_result sp5_buffer_init(sp5_buffer *buffer, uint8_t *storage, size_t capacity)
{
    if (buffer == NULL || (storage == NULL && capacity != 0)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    *buffer = (sp5_buffer){storage, capacity, 0};
    return sp5_success();
}

sp5_result sp5_buffer_append(sp5_buffer *buffer, sp5_bytes bytes)
{
    if (!buffer_valid(buffer) || (bytes.data == NULL && bytes.size != 0)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (bytes.size > buffer->capacity - buffer->size) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    if (bytes.size != 0) {
        memmove(buffer->data + buffer->size, bytes.data, bytes.size);
        buffer->size += bytes.size;
    }
    return sp5_success();
}

sp5_result sp5_buffer_reset(sp5_buffer *buffer)
{
    if (!buffer_valid(buffer)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    buffer->size = 0;
    return sp5_success();
}

/* Compute floor(remainder * 1,000,000 / divisor) without compiler extensions
 * or overflowing intermediates, even for a near-UINT64_MAX clock frequency.
 * Each of the 20 fixed steps keeps rest < divisor and quotient < 1,000,000. */
static uint64_t fractional_us(uint64_t remainder, uint64_t divisor)
{
    uint64_t quotient = 0;
    uint64_t rest = 0;
    for (uint32_t bit = 1U << 19; bit != 0; bit >>= 1) {
        quotient *= 2;
        if (rest >= divisor - rest) {
            rest -= divisor - rest;
            ++quotient;
        } else {
            rest += rest;
        }
        if ((1000000U & bit) != 0) {
            if (rest >= divisor - remainder) {
                rest -= divisor - remainder;
                ++quotient;
            } else {
                rest += remainder;
            }
        }
    }
    return quotient;
}

static sp5_result duration_failure(sp5_duration *duration, sp5_reason reason)
{
    if (duration != NULL) {
        *duration = (sp5_duration){.validity = SP5_VALIDITY_INVALID, .reason = reason};
    }
    return sp5_failure(reason);
}

sp5_result sp5_duration_between(const sp5_clock_description *clock, sp5_timestamp start,
                                sp5_timestamp end, sp5_duration *duration)
{
    if (clock == NULL || duration == NULL) {
        return duration_failure(duration, SP5_REASON_INVALID_ARGUMENT);
    }
    if (clock->validity != SP5_VALIDITY_VALID) {
        return duration_failure(duration, clock->reason == SP5_REASON_NONE ? SP5_REASON_CLOCK
                                                                           : clock->reason);
    }
    if (start.validity != SP5_VALIDITY_VALID || end.validity != SP5_VALIDITY_VALID) {
        sp5_reason reason = start.validity != SP5_VALIDITY_VALID ? start.reason : end.reason;
        return duration_failure(duration, reason == SP5_REASON_NONE ? SP5_REASON_CLOCK : reason);
    }
    if (!clock->monotonic || clock->ticks_per_second == 0 || clock->native_resolution_ticks == 0 ||
        start.domain != clock->domain || end.domain != clock->domain ||
        start.epoch != clock->epoch || end.epoch != clock->epoch || end.ticks < start.ticks) {
        return duration_failure(duration, SP5_REASON_CLOCK);
    }
    uint64_t ticks = end.ticks - start.ticks;
    uint64_t seconds = ticks / clock->ticks_per_second;
    uint64_t fraction = fractional_us(ticks % clock->ticks_per_second, clock->ticks_per_second);
    if (seconds > (UINT64_MAX - fraction) / 1000000 ||
        (clock->verified_uncertainty_ns.present &&
         clock->verified_uncertainty_ns.value > UINT64_MAX / 2)) {
        return duration_failure(duration, SP5_REASON_OVERFLOW);
    }
    *duration = (sp5_duration){.value_us = {true, seconds * 1000000 + fraction},
                               .quantization_exclusive_upper_bound_ns = 1000,
                               .validity = SP5_VALIDITY_VALID};
    if (clock->verified_uncertainty_ns.present) {
        duration->clock_uncertainty_ns =
            (sp5_optional_u64){true, clock->verified_uncertainty_ns.value * 2};
    }
    return sp5_success();
}

sp5_result sp5_log_write(sp5_buffer *buffer, sp5_log_event event, sp5_reason reason,
                         sp5_log_visibility visibility, sp5_bytes detail)
{
    const char *event_name;
    switch (event) {
    case SP5_LOG_SESSION_END:
        event_name = "session_end";
        break;
    case SP5_LOG_DIAGNOSTIC:
        event_name = "diagnostic";
        break;
    default:
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if ((visibility != SP5_LOG_PUBLIC && visibility != SP5_LOG_SECRET) ||
        (visibility == SP5_LOG_PUBLIC && detail.data == NULL && detail.size != 0)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    uint8_t storage[SP5_LOG_RECORD_CAPACITY];
    sp5_buffer record = {storage, sizeof(storage), 0};
    const char *reason_name = sp5_reason_name(reason);
    /* All prefixes are fixed vocabulary and the payload limit fits the record.
     * Propagate even internal bound failures rather than silently dropping logs. */
    sp5_result result =
        sp5_buffer_append(&record, (sp5_bytes){(const uint8_t *)event_name, strlen(event_name)});
    if (result.status == SP5_OK) {
        result = sp5_buffer_append(&record, (sp5_bytes){(const uint8_t *)" ", 1});
    }
    if (result.status == SP5_OK) {
        result = sp5_buffer_append(&record,
                                   (sp5_bytes){(const uint8_t *)reason_name, strlen(reason_name)});
    }
    if (result.status == SP5_OK) {
        result = sp5_buffer_append(&record, (sp5_bytes){(const uint8_t *)" ", 1});
    }
    if (result.status != SP5_OK) {
        return result;
    }
    if (visibility == SP5_LOG_SECRET) {
        result = sp5_buffer_append(&record, (sp5_bytes){(const uint8_t *)"[redacted]", 10});
    } else {
        size_t count = detail.size < SP5_LOG_PUBLIC_LIMIT ? detail.size : SP5_LOG_PUBLIC_LIMIT;
        for (size_t i = 0; i < count; ++i) {
            uint8_t value = detail.data[i];
            storage[record.size++] = value >= 32 && value <= 126 ? value : '?';
        }
        if (count != detail.size) {
            result = sp5_buffer_append(&record, (sp5_bytes){(const uint8_t *)"[truncated]", 11});
        }
    }
    if (result.status == SP5_OK) {
        result = sp5_buffer_append(&record, (sp5_bytes){(const uint8_t *)"\n", 1});
    }
    if (result.status != SP5_OK) {
        return result;
    }
    return sp5_buffer_append(buffer, (sp5_bytes){storage, record.size});
}
