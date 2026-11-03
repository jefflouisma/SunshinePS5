#ifndef SUNSHINE_PLATFORM_PS5_CLOCK_H
#define SUNSHINE_PLATFORM_PS5_CLOCK_H

#include "platform.h"
#include "runtime.h"

#include <time.h>

/* Adapter-local observation clock. The probe records CLOCK_MONOTONIC truncated
 * to integer microseconds; resolution/accuracy were not independently measured.
 * Domain/epoch are adapter labels so callers never compare these ticks to a
 * different clock. ticks_per_second is not claimed through this helper. */
#define SP5_PS5_CLOCK_DOMAIN UINT64_C(0x5053354d4f4e) /* "PS5MON" */
#define SP5_PS5_CLOCK_EPOCH UINT64_C(0)

static inline sp5_timestamp sp5_ps5_clock_unavailable(sp5_reason reason)
{
    return (sp5_timestamp){.validity = SP5_VALIDITY_INVALID, .reason = reason};
}

static inline sp5_result sp5_ps5_clock_now(sp5_timestamp *timestamp, uint64_t *microseconds)
{
    if (timestamp == NULL || microseconds == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1000000000L || (uint64_t)now.tv_sec > (UINT64_MAX - 999999) / 1000000) {
        *timestamp = sp5_ps5_clock_unavailable(SP5_REASON_CLOCK);
        *microseconds = 0;
        return sp5_failure(SP5_REASON_CLOCK);
    }
    uint64_t value = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
    *timestamp = (sp5_timestamp){.ticks = value,
                                 .domain = SP5_PS5_CLOCK_DOMAIN,
                                 .epoch = SP5_PS5_CLOCK_EPOCH,
                                 .validity = SP5_VALIDITY_VALID,
                                 .reason = SP5_REASON_NONE};
    *microseconds = value;
    return sp5_success();
}

static inline bool sp5_ps5_clock_matches(const sp5_timestamp *timestamp)
{
    return timestamp != NULL && timestamp->validity == SP5_VALIDITY_VALID &&
           timestamp->domain == SP5_PS5_CLOCK_DOMAIN && timestamp->epoch == SP5_PS5_CLOCK_EPOCH;
}

/* Media deadlines are adapter-clock timestamps. Equality is still on time. */
static inline sp5_result sp5_ps5_clock_check_deadline(sp5_timestamp deadline)
{
    if (!sp5_ps5_clock_matches(&deadline))
        return sp5_failure(SP5_REASON_CLOCK);
    sp5_timestamp now;
    uint64_t microseconds;
    sp5_result result = sp5_ps5_clock_now(&now, &microseconds);
    if (result.status != SP5_OK)
        return result;
    (void)microseconds;
    return now.ticks > deadline.ticks ? sp5_failure(SP5_REASON_TIMEOUT) : sp5_success();
}

#endif
