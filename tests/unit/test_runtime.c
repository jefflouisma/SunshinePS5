#include "runtime.h"

#include <stdio.h>
#include <string.h>

/* Unlike assert(), these checks remain active in optimized/NDEBUG builds. */
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            if (fputs(#condition "\n", stderr) == EOF) {                                           \
                return 2;                                                                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int buffers(void)
{
    uint8_t storage[6] = {0, 0, 0, 0, 0, 0xA5};
    sp5_buffer buffer;
    CHECK(sp5_buffer_init(&buffer, storage, 5).status == SP5_OK);
    CHECK(sp5_buffer_append(&buffer, (sp5_bytes){(const uint8_t *)"abc", 3}).status == SP5_OK);
    CHECK(buffer.size == 3);
    CHECK(sp5_buffer_append(&buffer, (sp5_bytes){(const uint8_t *)"def", 3}).reason ==
          SP5_REASON_CAPACITY);
    CHECK(buffer.size == 3 && storage[3] == 0 && storage[5] == 0xA5);
    CHECK(sp5_buffer_append(&buffer, (sp5_bytes){NULL, 1}).reason == SP5_REASON_INVALID_ARGUMENT);
    CHECK(sp5_buffer_append(&buffer, (sp5_bytes){storage, SIZE_MAX}).reason == SP5_REASON_CAPACITY);
    CHECK(sp5_buffer_append(&buffer, (sp5_bytes){storage, 2}).status == SP5_OK);
    CHECK(memcmp(storage, "abcab", 5) == 0 && storage[5] == 0xA5);
    CHECK(sp5_buffer_reset(&buffer).status == SP5_OK && buffer.size == 0);
    buffer.size = 6;
    CHECK(sp5_buffer_append(&buffer, (sp5_bytes){NULL, 0}).reason == SP5_REASON_INVALID_ARGUMENT);
    CHECK(sp5_buffer_init(NULL, storage, 5).reason == SP5_REASON_INVALID_ARGUMENT);
    CHECK(sp5_buffer_init(&buffer, NULL, 1).reason == SP5_REASON_INVALID_ARGUMENT);
    CHECK(sp5_buffer_init(&buffer, NULL, 0).status == SP5_OK);
    CHECK(sp5_buffer_append(&buffer, (sp5_bytes){NULL, 0}).status == SP5_OK);
    CHECK(sp5_buffer_reset(NULL).status == SP5_ERROR);
    return 0;
}

static sp5_clock_description clock_description(uint64_t frequency)
{
    return (sp5_clock_description){.domain = 1,
                                   .epoch = 2,
                                   .ticks_per_second = frequency,
                                   .native_resolution_ticks = 1,
                                   .monotonic = true,
                                   .verified_uncertainty_ns = {true, 7},
                                   .validity = SP5_VALIDITY_VALID};
}

static int clocks(void)
{
    sp5_clock_description clock = clock_description(3000000);
    sp5_timestamp start = {.ticks = 10, .domain = 1, .epoch = 2, .validity = SP5_VALIDITY_VALID};
    sp5_timestamp end = start;
    sp5_duration duration;
    end.ticks += 5;
    CHECK(sp5_duration_between(&clock, start, end, &duration).status == SP5_OK);
    CHECK(duration.value_us.present && duration.value_us.value == 1);
    CHECK(duration.clock_uncertainty_ns.present && duration.clock_uncertainty_ns.value == 14);
    CHECK(duration.quantization_exclusive_upper_bound_ns == 1000);
    CHECK(!duration.lower_bound_us.present && !duration.upper_bound_us.present);
    CHECK(sp5_duration_between(&clock, start, start, &duration).status == SP5_OK);
    CHECK(duration.value_us.present && duration.value_us.value == 0);
    clock.verified_uncertainty_ns.present = false;
    CHECK(sp5_duration_between(&clock, start, end, &duration).status == SP5_OK);
    CHECK(!duration.clock_uncertainty_ns.present);
    end.ticks = start.ticks - 1;
    CHECK(sp5_duration_between(&clock, start, end, &duration).reason == SP5_REASON_CLOCK);
    CHECK(!duration.value_us.present && duration.validity == SP5_VALIDITY_INVALID);
    end = start;
    end.epoch++;
    CHECK(sp5_duration_between(&clock, start, end, &duration).reason == SP5_REASON_CLOCK);
    end = start;
    end.domain++;
    CHECK(sp5_duration_between(&clock, start, end, &duration).reason == SP5_REASON_CLOCK);
    end = start;
    end.validity = SP5_VALIDITY_UNAVAILABLE;
    end.reason = SP5_REASON_STAGE_UNAVAILABLE;
    CHECK(sp5_duration_between(&clock, start, end, &duration).reason ==
          SP5_REASON_STAGE_UNAVAILABLE);
    end = start;
    clock.monotonic = false;
    CHECK(sp5_duration_between(&clock, start, end, &duration).reason == SP5_REASON_CLOCK);
    clock = clock_description(0);
    CHECK(sp5_duration_between(&clock, start, end, &duration).reason == SP5_REASON_CLOCK);
    clock = clock_description(1);
    clock.native_resolution_ticks = 0;
    CHECK(sp5_duration_between(&clock, start, end, &duration).reason == SP5_REASON_CLOCK);
    clock = clock_description(1);
    start.ticks = 0;
    end.ticks = UINT64_MAX;
    CHECK(sp5_duration_between(&clock, start, end, &duration).reason == SP5_REASON_OVERFLOW);
    CHECK(!duration.value_us.present);
    clock = clock_description(1000000);
    CHECK(sp5_duration_between(&clock, start, end, &duration).status == SP5_OK);
    CHECK(duration.value_us.value == UINT64_MAX);
    clock = clock_description(UINT64_MAX);
    end.ticks = UINT64_MAX - 1;
    CHECK(sp5_duration_between(&clock, start, end, &duration).status == SP5_OK);
    CHECK(duration.value_us.value == 999999);
    clock.verified_uncertainty_ns.value = UINT64_MAX;
    CHECK(sp5_duration_between(&clock, start, end, &duration).reason == SP5_REASON_OVERFLOW);
    CHECK(!duration.value_us.present);
    CHECK(sp5_duration_between(NULL, start, end, &duration).reason == SP5_REASON_INVALID_ARGUMENT);
    CHECK(!duration.value_us.present);
    CHECK(sp5_duration_between(&clock, start, end, NULL).reason == SP5_REASON_INVALID_ARGUMENT);
    /* Exact small-domain oracle exercises non-divisible conversion frequencies. */
    for (uint64_t frequency = 1; frequency < 97; ++frequency) {
        clock = clock_description(frequency);
        for (uint64_t ticks = 0; ticks < 200; ++ticks) {
            end.ticks = ticks;
            CHECK(sp5_duration_between(&clock, start, end, &duration).status == SP5_OK);
            CHECK(duration.value_us.value == ticks * 1000000 / frequency);
        }
    }
    return 0;
}

static int logging(void)
{
    uint8_t storage[512] = {0};
    sp5_buffer buffer;
    CHECK(sp5_buffer_init(&buffer, storage, sizeof(storage) - 1).status == SP5_OK);
    CHECK(sp5_log_write(&buffer, SP5_LOG_SESSION_END, SP5_REASON_TIMEOUT, SP5_LOG_SECRET,
                        (sp5_bytes){(const uint8_t *)"PIN=4321 token=secret", 21})
              .status == SP5_OK);
    CHECK(strstr((char *)storage, "session_end timeout [redacted]\n") != NULL);
    CHECK(strstr((char *)storage, "4321") == NULL && strstr((char *)storage, "secret") == NULL);
    CHECK(sp5_log_write(&buffer, SP5_LOG_DIAGNOSTIC, SP5_REASON_NONE, SP5_LOG_PUBLIC,
                        (sp5_bytes){(const uint8_t *)"name\n\r\033", 7})
              .status == SP5_OK);
    CHECK(strstr((char *)storage, "name???\n") != NULL);
    uint8_t large[1000];
    memset(large, 'x', sizeof(large));
    size_t previous = buffer.size;
    CHECK(sp5_log_write(&buffer, SP5_LOG_DIAGNOSTIC, SP5_REASON_NONE, SP5_LOG_PUBLIC,
                        (sp5_bytes){large, sizeof(large)})
              .status == SP5_OK);
    CHECK(buffer.size - previous <= SP5_LOG_RECORD_CAPACITY);
    CHECK(strstr((char *)storage, "[truncated]") != NULL);
    buffer.capacity = buffer.size;
    previous = buffer.size;
    CHECK(sp5_log_write(&buffer, SP5_LOG_SESSION_END, SP5_REASON_PLATFORM, SP5_LOG_SECRET,
                        (sp5_bytes){NULL, SIZE_MAX})
              .reason == SP5_REASON_CAPACITY);
    CHECK(buffer.size == previous);
    CHECK(sp5_log_write(&buffer, SP5_LOG_EVENT_INVALID, SP5_REASON_NONE, SP5_LOG_PUBLIC,
                        (sp5_bytes){NULL, 0})
              .reason == SP5_REASON_INVALID_ARGUMENT);
    CHECK(sp5_log_write(&buffer, SP5_LOG_DIAGNOSTIC, SP5_REASON_NONE, SP5_LOG_VISIBILITY_INVALID,
                        (sp5_bytes){NULL, 0})
              .reason == SP5_REASON_INVALID_ARGUMENT);
    CHECK(sp5_log_write(&buffer, SP5_LOG_DIAGNOSTIC, SP5_REASON_NONE, SP5_LOG_PUBLIC,
                        (sp5_bytes){NULL, 1})
              .reason == SP5_REASON_INVALID_ARGUMENT);
    for (int reason = SP5_REASON_NONE; reason <= SP5_REASON_OWNERSHIP; ++reason) {
        CHECK(strcmp(sp5_reason_name((sp5_reason)reason), "unknown") != 0);
    }
    CHECK(strcmp(sp5_reason_name((sp5_reason)(SP5_REASON_OWNERSHIP + 1)), "unknown") == 0);
    CHECK(sp5_success().status == SP5_OK && sp5_success().reason == SP5_REASON_NONE);
    CHECK(sp5_failure(SP5_REASON_NONE).status == SP5_ERROR);
    CHECK(sp5_failure(SP5_REASON_NONE).reason == SP5_REASON_INVALID_ARGUMENT);
    return 0;
}

int main(void)
{
    if (buffers() || clocks() || logging()) {
        return 1;
    }
    if (puts("runtime: buffers, clocks, redaction and failure paths passed") == EOF) {
        return 2;
    }
    return 0;
}
