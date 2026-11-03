/* Nonshipping T044/R5 measurement admission and optional owned-thread idle probe.
 * ABI source: ps5-payload-dev/sdk a7685ce8e97ce1f8a4f38253a41d54241c9124f1,
 * target/include/{time.h,stdlib.h,sys/resource.h}; CLOCK_*_CPUTIME_ID are declared
 * there, not guessed. Runtime support is tested and may be unavailable.
 * No media/input calls: research-r3-findings.md admits no complete path.
 * Stock SDK crt/crt.c -> crt/patch.c is the accepted privileged bootstrap;
 * this probe adds no credential writes, hooks, scheduling changes or offsets.
 * Default mode exits 2 (Incomplete); -DBUDGET_IDLE explicitly enables one
 * ten-second diagnostic. This is NOT SunshinePS5 idle or streaming evidence.
 * -DBUDGET_CLOCK_CHARACTERIZE instead enables two 250-ms clock observations.
 * -DBUDGET_THREADS enables two rounds of concurrent sleep/active owned workers.
 * -DBUDGET_WINDOW encloses one coordinator-defined ten-second worker window.
 * POSIX thread ABI follows dependencies.c and the pinned SDK pthread.h.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(BUDGET_THREADS) || defined(BUDGET_WINDOW) || defined(BUDGET_TEST)
#include "budget_test.h"
#endif
#if defined(BUDGET_THREADS) || defined(BUDGET_WINDOW)
#include <pthread.h>
#endif

#ifdef BUDGET_TEST
#include "budget_test.h"
#define clock_gettime r5_budget_test_clock
#define nanosleep r5_budget_test_sleep
#endif

#ifndef BUDGET_TEST
static void admission(void)
{
    puts("R5_SCOPE kind=nonshipping_probe verdict=incomplete shipping_host=absent");
    puts("R5_CEILINGS total_bytes=268435456 media_bytes=134217728 metrics_bytes=83886080 "
         "core_bytes=50331648 streaming_cpu_ns_per_10s=10000000000 "
         "idle_cpu_ns_per_10s=500000000 measured=no");
    static const char *const missing[] = {"capture_start_to_complete",
                                          "encode_start_to_complete",
                                          "packetize_start_to_complete",
                                          "last_packet_handoff",
                                          "full_pipeline_p99",
                                          "protocol_processing_p99",
                                          "host_frame_drops",
                                          "input_host_arrival_to_console_delivery",
                                          "media_driver_allocations",
                                          "metrics_allocations",
                                          "core_tls_web_stacks_allocations",
                                          "aggregate_streaming_thread_cpu",
                                          "host_idle_thread_cpu",
                                          "foreground_game_impact_five_games",
                                          "sdr_1080p60_30minute_baseline",
                                          "hdr_1080p60_30minute_baseline",
                                          "supported_minimum_bitrate",
                                          "supported_maximum_bitrate"};
    for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); ++i) {
        printf("R5_REQUIRED domain=%s value=absent status=unresolved "
               "reason=no_established_complete_R3_streaming_path\n",
               missing[i]);
    }
}
#endif

#if defined(BUDGET_IDLE) || defined(BUDGET_CLOCK_CHARACTERIZE) || defined(BUDGET_THREADS) ||       \
    defined(BUDGET_WINDOW) || defined(BUDGET_TEST)
static bool nanoseconds(clockid_t clock, uint64_t *value)
{
    struct timespec now;
    if (clock_gettime(clock, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1000000000L || (uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(1000000000)) {
        return false;
    }
    uint64_t seconds = (uint64_t)now.tv_sec * UINT64_C(1000000000);
    if (UINT64_MAX - seconds < (uint64_t)now.tv_nsec) {
        return false;
    }
    *value = seconds + (uint64_t)now.tv_nsec;
    return true;
}
#endif

#ifdef BUDGET_IDLE
static int idle(void)
{
    enum { REQUESTED_BYTES = 65536 };
    volatile unsigned char *pool = malloc(REQUESTED_BYTES);
    if (pool == NULL) {
        puts("R5_DIAGNOSTIC allocation=failed");
        return 1;
    }
    for (size_t i = 0; i < REQUESTED_BYTES; ++i) {
        pool[i] = (unsigned char)i;
    }
    printf("R5_DIAGNOSTIC attributable_requested_heap_bytes=%u "
           "allocator_overhead=unknown runtime_heap=unknown main_stack=unknown "
           "media_driver_bytes=unknown host_budget_verdict=unresolved\n",
           REQUESTED_BYTES);
    uint64_t wall_begin, wall_end, thread_begin, thread_end, process_begin, process_end;
    bool begin_valid = nanoseconds(CLOCK_MONOTONIC, &wall_begin);
    bool thread_valid = nanoseconds(CLOCK_THREAD_CPUTIME_ID, &thread_begin);
    bool process_valid = nanoseconds(CLOCK_PROCESS_CPUTIME_ID, &process_begin);
    struct timespec delay = {10, 0}, remaining;
    int status;
    do {
        status = nanosleep(&delay, &remaining);
        if (status != 0 && errno == EINTR) {
            delay = remaining;
        }
    } while (status != 0 && errno == EINTR);
    bool end_valid = nanoseconds(CLOCK_MONOTONIC, &wall_end);
    thread_valid = nanoseconds(CLOCK_THREAD_CPUTIME_ID, &thread_end) && thread_valid;
    process_valid = nanoseconds(CLOCK_PROCESS_CPUTIME_ID, &process_end) && process_valid;
    free((void *)pool);
    if (status != 0 || !begin_valid || !end_valid || wall_end < wall_begin) {
        puts("R5_DIAGNOSTIC clock_or_wait=failed allocation_released=yes");
        return 1;
    }
    printf("R5_DIAGNOSTIC wall_window_ns=%" PRIu64 " requested_window_ns=10000000000 "
           "allocation_released=yes exact_10s_bucket=no workload=sleep_only\n",
           wall_end - wall_begin);
    thread_valid = thread_valid && thread_end >= thread_begin;
    process_valid = process_valid && process_end >= process_begin;
    if (thread_valid) {
        printf("R5_DIAGNOSTIC owned_main_thread_cpu_ns=%" PRIu64 " owned_threads=1\n",
               thread_end - thread_begin);
    } else {
        puts("R5_DIAGNOSTIC owned_main_thread_cpu_ns=absent reason=clock_unavailable_or_reversed");
    }
    if (process_valid) {
        printf("R5_DIAGNOSTIC process_cpu_ns=%" PRIu64 " scope=probe_process\n",
               process_end - process_begin);
    } else {
        puts("R5_DIAGNOSTIC process_cpu_ns=absent reason=clock_unavailable_or_reversed");
    }
    puts("R5_DIAGNOSTIC host_streaming_or_idle_budget=unresolved "
         "reason=probe_sleep_is_not_host_workload");
    return thread_valid && process_valid ? 2 : 1;
}
#endif

#if defined(BUDGET_CLOCK_CHARACTERIZE) || defined(BUDGET_TEST)
struct clock_phase {
    uint64_t begin[3], end[3], delta[3];
    uint32_t iterations;
    bool deadline;
    bool sampled;
};

static bool clock_snapshot(uint64_t values[3])
{
    return nanoseconds(CLOCK_MONOTONIC, &values[0]) &&
           nanoseconds(CLOCK_THREAD_CPUTIME_ID, &values[1]) &&
           nanoseconds(CLOCK_PROCESS_CPUTIME_ID, &values[2]);
}

static int clock_phase(bool active, uint64_t duration_ns, uint32_t max_iterations,
                       struct clock_phase *result)
{
    *result = (struct clock_phase){0};
    if (duration_ns == 0 || max_iterations == 0 || !clock_snapshot(result->begin)) {
        return 1;
    }
    uint64_t previous = result->begin[0];
    volatile uint64_t work = 1;
    while (result->iterations < max_iterations) {
        uint64_t now;
        if (!nanoseconds(CLOCK_MONOTONIC, &now) || now < previous) {
            return 1;
        }
        previous = now;
        if (now - result->begin[0] >= duration_ns) {
            result->deadline = true;
            break;
        }
        ++result->iterations;
        if (active) {
            for (unsigned i = 0; i < 256; ++i) {
                work = work * UINT64_C(1664525) + UINT64_C(1013904223);
            }
        } else {
            const struct timespec delay = {0, 1000000L};
            /* An interrupted wait consumes an iteration and rechecks the deadline. */
            if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
                return 1;
            }
        }
    }
    if (!clock_snapshot(result->end) || result->end[0] < previous) {
        return 1;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (result->end[i] < result->begin[i]) {
            return 1;
        }
        result->delta[i] = result->end[i] - result->begin[i];
    }
    result->sampled = true;
    /* Cap exhaustion stays a failed observation even if its final clock advances. */
    return result->deadline ? 2 : 1;
}
#endif

#ifdef BUDGET_CLOCK_CHARACTERIZE
static int characterize(void)
{
    const uint64_t duration_ns = UINT64_C(250000000);
    const char *const names[] = {"monotonic", "thread_cpu", "process_cpu"};
    puts("R5_CLOCK_BEGIN phases=sleep,active requested_phase_ns=250000000 "
         "sleep_iteration_cap=1000 active_iteration_cap=1000000 "
         "active_operations_per_iteration=256 "
         "clock_order=monotonic,thread,process atomic_snapshot=no "
         "platform_call_deadline=unknown owned_threads=1");
    for (unsigned phase = 0; phase < 2; ++phase) {
        struct clock_phase result;
        int status = clock_phase(phase != 0, duration_ns, phase == 0 ? 1000 : 1000000, &result);
        const char *name = phase == 0 ? "sleep" : "active";
        printf("R5_CLOCK_PHASE phase=%s iterations=%" PRIu32 " termination=%s result=%s\n", name,
               result.iterations, result.deadline ? "deadline" : "cap_or_error",
               status == 2 ? "observed" : "failed");
        for (unsigned i = 0; result.sampled && i < 3; ++i) {
            printf("R5_CLOCK_RAW phase=%s clock=%s begin_ns=%" PRIu64 " end_ns=%" PRIu64
                   " delta_ns=%" PRIu64 "\n",
                   name, names[i], result.begin[i], result.end[i], result.delta[i]);
        }
        if (status != 2) {
            puts("R5_CLOCK_RESULT status=incomplete observation=failed host_budget=unresolved");
            return 1;
        }
    }
    puts("R5_CLOCK_RESULT status=incomplete observation=complete "
         "clock_semantics=unverified host_budget=unresolved");
    return 2;
}
#endif

#if defined(BUDGET_THREADS) || defined(BUDGET_TEST)
bool r5_thread_totals(const struct r5_thread_sample samples[2], uint64_t *sum)
{
    uint64_t total = 0, latest = 0, earliest = UINT64_MAX, longest = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const struct r5_thread_sample *s = &samples[i];
        if (!s->complete || s->begin[2] < s->begin[0] || s->end[2] < s->end[0] ||
            s->end[0] < s->begin[2] || s->end[1] < s->begin[1] ||
            s->begin[2] - s->begin[0] > UINT64_C(5000000) ||
            s->end[2] - s->end[0] > UINT64_C(5000000))
            return false;
        uint64_t elapsed = s->end[0] - s->begin[2];
        uint64_t cpu = s->end[1] - s->begin[1];
        if (elapsed < UINT64_C(250000000) || UINT64_MAX - total < cpu)
            return false;
        total += cpu;
        if (s->begin[2] > latest)
            latest = s->begin[2];
        if (s->end[0] < earliest)
            earliest = s->end[0];
        if (elapsed > longest)
            longest = elapsed;
    }
    /* At least 90% of the longest interior interval must overlap. This is a
     * diagnostic comparability gate, never an exact common-window CPU total. */
    if (earliest < latest || longest - (earliest - latest) > longest / 10)
        return false;
    *sum = total;
    return true;
}

bool r5_thread_run(const struct r5_thread_api *api, struct r5_thread_run *run, bool swap,
                   uint64_t *sum)
{
    if (!api || !api->create || !api->join || !run || !sum)
        return false;
    *run = (struct r5_thread_run){0};
    atomic_init(&run->start, false);
    bool ok = true;
    for (unsigned i = 0; i < 2; ++i) {
        run->workers[i].start = &run->start;
        run->workers[i].active = (i != 0) != swap;
        if (api->create(api->context, i, &run->workers[i]) != 0) {
            ok = false;
            break;
        }
        run->created[i] = true;
    }
    atomic_store_explicit(&run->start, true, memory_order_release);
    for (unsigned i = 0; i < 2; ++i) {
        if (run->created[i]) {
            run->joined[i] = api->join(api->context, i) == 0;
            ok = run->joined[i] && ok;
        }
    }
    if (!ok)
        return false;
    const struct r5_thread_sample samples[2] = {run->workers[0].sample, run->workers[1].sample};
    return r5_thread_totals(samples, sum);
}
#endif

#if defined(BUDGET_THREADS) || defined(BUDGET_TEST)
static bool thread_snapshot(uint64_t sample[3])
{
    return nanoseconds(CLOCK_MONOTONIC, &sample[0]) &&
           nanoseconds(CLOCK_THREAD_CPUTIME_ID, &sample[1]) &&
           nanoseconds(CLOCK_MONOTONIC, &sample[2]);
}

static void *thread_observe(void *argument)
{
    struct r5_thread_worker *worker = argument;
    const struct timespec delay = {0, 1000000L};
    unsigned waits = 0;
    while (!atomic_load_explicit(worker->start, memory_order_acquire)) {
        if (++waits > 1000 || (nanosleep(&delay, NULL) != 0 && errno != EINTR))
            return NULL;
    }
    if (!thread_snapshot(worker->sample.begin))
        return NULL;
    uint64_t previous = worker->sample.begin[2];
    volatile uint64_t work = 1;
    bool deadline = false;
    for (unsigned i = 0; i < (worker->active ? 1000000U : 1000U); ++i) {
        uint64_t now;
        if (!nanoseconds(CLOCK_MONOTONIC, &now) || now < previous)
            return NULL;
        previous = now;
        if (now - worker->sample.begin[2] >= UINT64_C(250000000)) {
            deadline = true;
            break;
        }
        if (worker->active) {
            for (unsigned j = 0; j < 256; ++j)
                work = work * UINT64_C(1664525) + UINT64_C(1013904223);
        } else if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
            return NULL;
        }
    }
    worker->sample.complete =
        deadline && thread_snapshot(worker->sample.end) && worker->sample.end[0] >= previous;
    return NULL;
}

#ifdef BUDGET_TEST
void r5_budget_test_worker(struct r5_thread_worker *worker)
{
    (void)thread_observe(worker);
}
#endif
#endif

#ifdef BUDGET_THREADS
struct thread_handles {
    pthread_t values[2];
};

static int thread_create(void *context, unsigned index, struct r5_thread_worker *worker)
{
    struct thread_handles *threads = context;
    return pthread_create(&threads->values[index], NULL, thread_observe, worker);
}

static int thread_join(void *context, unsigned index)
{
    struct thread_handles *threads = context;
    return pthread_join(threads->values[index], NULL);
}

static int characterize_threads(void)
{
    /* Static lifetime preserves worker arguments if a join has uncertain outcome.
     * A failed round exits immediately and never reuses that storage. */
    static struct r5_thread_run run;
    static struct thread_handles threads;
    const struct r5_thread_api api = {thread_create, thread_join, &threads};
    puts("R5_THREADS_BEGIN workers=2 rounds=2 phase_ns=250000000 "
         "clock_order=monotonic,thread_cpu,monotonic start_wait_cap=1000 "
         "sleep_iteration_cap=1000 active_iteration_cap=1000000 "
         "platform_call_deadline=unknown exact_common_window=no");
    for (unsigned round = 0; round < 2; ++round) {
        uint64_t sum = 0;
        bool ok = r5_thread_run(&api, &run, round != 0, &sum);
        for (unsigned i = 0; i < 2; ++i) {
            printf("R5_THREAD round=%u worker=%u active=%d created=%d joined=%d", round, i,
                   (int)run.workers[i].active, (int)run.created[i], (int)run.joined[i]);
            /* Reading an unjoined worker's fields would itself be a data race. */
            if (run.joined[i]) {
                const struct r5_thread_sample *s = &run.workers[i].sample;
                printf(" complete=%d begin_before_ns=%" PRIu64 " begin_cpu_ns=%" PRIu64
                       " begin_after_ns=%" PRIu64 " end_before_ns=%" PRIu64 " end_cpu_ns=%" PRIu64
                       " end_after_ns=%" PRIu64,
                       (int)s->complete, s->begin[0], s->begin[1], s->begin[2], s->end[0],
                       s->end[1], s->end[2]);
            }
            putchar('\n');
        }
        if (!ok) {
            puts("R5_THREADS_RESULT observation=failed status=incomplete host_budget=unresolved");
            return 1;
        }
        printf("R5_THREADS_SUM round=%u cpu_ns=%" PRIu64
               " scope=two_owned_worker_intervals common_window=no\n",
               round, sum);
    }
    puts("R5_THREADS_RESULT observation=complete status=incomplete "
         "thread_isolation=requires_independent_comparison host_budget=unresolved");
    return 2;
}
#endif

#if defined(BUDGET_WINDOW) || defined(BUDGET_TEST)
bool r5_window_totals(const struct r5_thread_sample samples[2], uint64_t begin_ns, uint64_t end_ns,
                      struct r5_window_result *result)
{
    if (!samples || !result || end_ns < begin_ns || end_ns - begin_ns != R5_WINDOW_NS)
        return false;
    struct r5_window_result total = {0};
    uint64_t latest_begin = 0, earliest_begin = UINT64_MAX;
    uint64_t earliest_end = UINT64_MAX, latest_end = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const struct r5_thread_sample *s = &samples[i];
        if (!s->complete || s->begin[2] < s->begin[0] || s->end[2] < s->end[0] ||
            s->begin[2] > begin_ns || s->end[0] < end_ns || s->end[1] < s->begin[1] ||
            begin_ns - s->begin[0] > R5_WINDOW_MARGIN_NS ||
            s->end[2] - end_ns > R5_WINDOW_MARGIN_NS)
            return false;
        uint64_t cpu = s->end[1] - s->begin[1];
        if (UINT64_MAX - total.enclosing_cpu_ns < cpu)
            return false;
        total.enclosing_cpu_ns += cpu;
        /* The CPU instants lie somewhere inside the monotonic brackets. */
        total.outside_window_ns += begin_ns - s->begin[0] + s->end[2] - end_ns;
        if (s->begin[2] > latest_begin)
            latest_begin = s->begin[2];
        if (s->begin[0] < earliest_begin)
            earliest_begin = s->begin[0];
        if (s->end[0] < earliest_end)
            earliest_end = s->end[0];
        if (s->end[2] > latest_end)
            latest_end = s->end[2];
    }
    total.common_coverage_ns = earliest_end - latest_begin;
    total.begin_skew_ns = latest_begin - earliest_begin;
    total.end_skew_ns = latest_end - earliest_end;
    *result = total;
    return true;
}

static bool window_coordinator_now(const struct r5_window_api *api, struct r5_window_run *run,
                                   uint64_t *now)
{
    if (!api->now(api->context, now) ||
        (run->coordinator_sampled && *now < run->coordinator_last_ns))
        return false;
    run->coordinator_last_ns = *now;
    run->coordinator_sampled = true;
    return true;
}

struct window_poll_limit {
    uint64_t duration_ns;
    unsigned iterations;
};

static bool window_poll(const struct r5_window_api *api, struct r5_window_run *run, bool completion,
                        struct window_poll_limit limit)
{
    uint64_t begin, previous;
    if (!window_coordinator_now(api, run, &begin))
        return false;
    previous = begin;
    for (unsigned iteration = 0; iteration < limit.iterations; ++iteration) {
        uint64_t now;
        if (!window_coordinator_now(api, run, &now) || now < previous ||
            now - begin > limit.duration_ns)
            return false;
        previous = now;
        bool all = true;
        for (unsigned i = 0; i < 2; ++i) {
            if (run->created[i] &&
                !atomic_load_explicit(completion ? &run->workers[i].done : &run->workers[i].ready,
                                      memory_order_acquire))
                all = false;
        }
        if (all)
            return true;
        if (!api->wait(api->context))
            return false;
    }
    return false;
}

bool r5_window_run(const struct r5_window_api *api, struct r5_window_run *run,
                   struct r5_window_result *result)
{
    if (!api || !api->create || !api->join || !api->now || !api->wait || !run || !result)
        return false;
    *run = (struct r5_window_run){0};
    atomic_init(&run->published, false);
    atomic_init(&run->cancel, false);
    for (unsigned i = 0; i < 2; ++i) {
        atomic_init(&run->workers[i].ready, false);
        atomic_init(&run->workers[i].done, false);
        run->workers[i].run = run;
        run->workers[i].active = i != 0;
    }
    bool ok = true;
    for (unsigned i = 0; i < 2; ++i) {
        if (api->create(api->context, i, &run->workers[i]) != 0) {
            ok = false;
            break;
        }
        run->created[i] = true;
    }
    const struct window_poll_limit short_limit = {UINT64_C(1000000000), 2000};
    const struct window_poll_limit completion_limit = {UINT64_C(11000000000), 20000};
    ok = ok && window_poll(api, run, false, short_limit);
    uint64_t now;
    if (ok) {
        ok = window_coordinator_now(api, run, &now) &&
             now <= UINT64_MAX - R5_WINDOW_NS - UINT64_C(50000000);
        if (ok) {
            run->begin_ns = now + UINT64_C(50000000);
            run->end_ns = run->begin_ns + R5_WINDOW_NS;
        }
    }
    if (!ok)
        atomic_store_explicit(&run->cancel, true, memory_order_release);
    atomic_store_explicit(&run->published, true, memory_order_release);
    bool completed = window_poll(api, run, true, ok ? completion_limit : short_limit);
    if (!completed) {
        atomic_store_explicit(&run->cancel, true, memory_order_release);
        /* One bounded cancellation grace, never an unbounded join on a live worker. */
        (void)window_poll(api, run, true, short_limit);
        ok = false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (!run->created[i])
            continue;
        if (!atomic_load_explicit(&run->workers[i].done, memory_order_acquire)) {
            ok = false;
            continue;
        }
        run->joined[i] = api->join(api->context, i) == 0;
        ok = run->joined[i] && ok;
    }
    if (!ok)
        return false;
    const struct r5_thread_sample samples[2] = {run->workers[0].sample, run->workers[1].sample};
    return r5_window_totals(samples, run->begin_ns, run->end_ns, result);
}

static bool window_wait(void *context)
{
    (void)context;
    const struct timespec delay = {0, 1000000L};
    return nanosleep(&delay, NULL) == 0 || errno == EINTR;
}

static bool window_snapshot(uint64_t sample[3])
{
    return nanoseconds(CLOCK_MONOTONIC, &sample[0]) &&
           nanoseconds(CLOCK_THREAD_CPUTIME_ID, &sample[1]) &&
           nanoseconds(CLOCK_MONOTONIC, &sample[2]);
}

static bool window_until(struct r5_window_worker *worker, uint64_t target, bool active,
                         uint64_t *previous)
{
    volatile uint64_t work = 1;
    const unsigned cap = active ? 40000000U : 20000U;
    for (unsigned i = 0; i < cap; ++i) {
        uint64_t now;
        if (atomic_load_explicit(&worker->run->cancel, memory_order_acquire) ||
            !nanoseconds(CLOCK_MONOTONIC, &now) || now < *previous)
            return false;
        *previous = now;
        if (now >= target)
            return true;
        if (active) {
            for (unsigned j = 0; j < 256; ++j)
                work = work * UINT64_C(1664525) + UINT64_C(1013904223);
        } else if (!window_wait(NULL))
            return false;
    }
    return false;
}

static void *window_observe(void *argument)
{
    struct r5_window_worker *worker = argument;
    atomic_store_explicit(&worker->ready, true, memory_order_release);
    unsigned waits = 0;
    while (!atomic_load_explicit(&worker->run->published, memory_order_acquire)) {
        if (++waits > 2000 || !window_wait(NULL))
            goto done;
    }
    if (atomic_load_explicit(&worker->run->cancel, memory_order_acquire))
        goto done;
    if (worker->run->begin_ns < UINT64_C(2000000) || worker->run->end_ns < worker->run->begin_ns ||
        worker->run->end_ns - worker->run->begin_ns != R5_WINDOW_NS)
        goto done;
    uint64_t previous;
    if (!nanoseconds(CLOCK_MONOTONIC, &previous) ||
        !window_until(worker, worker->run->begin_ns - UINT64_C(2000000), false, &previous) ||
        !window_snapshot(worker->sample.begin) || worker->sample.begin[0] < previous ||
        worker->sample.begin[2] < worker->sample.begin[0] ||
        worker->sample.begin[2] > worker->run->begin_ns)
        goto done;
    previous = worker->sample.begin[2];
    if (!window_until(worker, worker->run->end_ns, worker->active, &previous) ||
        !window_snapshot(worker->sample.end) || worker->sample.end[0] < previous)
        goto done;
    worker->sample.complete = true;
done:
    atomic_store_explicit(&worker->done, true, memory_order_release);
    return NULL;
}

#ifdef BUDGET_TEST
void r5_budget_test_window_worker(struct r5_window_worker *worker)
{
    (void)window_observe(worker);
}
#endif
#endif

#ifdef BUDGET_WINDOW
struct window_handles {
    pthread_t values[2];
};
static int window_create(void *context, unsigned index, struct r5_window_worker *worker)
{
    return pthread_create(&((struct window_handles *)context)->values[index], NULL, window_observe,
                          worker);
}
static int window_join(void *context, unsigned index)
{
    return pthread_join(((struct window_handles *)context)->values[index], NULL);
}
static bool window_now(void *context, uint64_t *now)
{
    (void)context;
    return nanoseconds(CLOCK_MONOTONIC, now);
}
static int characterize_window(void)
{
    /* Keep arguments valid through process exit if completion/join is uncertain. */
    static struct r5_window_run run;
    static struct window_handles handles;
    const struct r5_window_api api = {window_create, window_join, window_now, window_wait,
                                      &handles};
    struct r5_window_result result;
    puts("R5_WINDOW_BEGIN workers=2 coordinator_counted=no target_duration_ns=10000000000 "
         "edge_margin_limit_ns=5000000 ready_timeout_ns=1000000000 "
         "completion_timeout_ns=11000000000 platform_call_deadline=unknown");
    bool ok = r5_window_run(&api, &run, &result);
    printf("R5_WINDOW_TARGET begin_ns=%" PRIu64 " end_ns=%" PRIu64 "\n", run.begin_ns, run.end_ns);
    for (unsigned i = 0; i < 2; ++i) {
        printf("R5_WINDOW_WORKER worker=%u active=%d created=%d joined=%d", i,
               (int)run.workers[i].active, (int)run.created[i], (int)run.joined[i]);
        if (run.joined[i]) {
            const struct r5_thread_sample *s = &run.workers[i].sample;
            printf(" complete=%d begin_before_ns=%" PRIu64 " begin_cpu_ns=%" PRIu64
                   " begin_after_ns=%" PRIu64 " end_before_ns=%" PRIu64 " end_cpu_ns=%" PRIu64
                   " end_after_ns=%" PRIu64,
                   (int)s->complete, s->begin[0], s->begin[1], s->begin[2], s->end[0], s->end[1],
                   s->end[2]);
        }
        putchar('\n');
    }
    if (ok)
        printf("R5_WINDOW_SUM enclosing_cpu_ns=%" PRIu64 " outside_window_wall_bound_ns=%" PRIu64
               " common_coverage_ns=%" PRIu64 " begin_skew_bound_ns=%" PRIu64
               " end_skew_bound_ns=%" PRIu64 " exact_common_window_cpu=no\n",
               result.enclosing_cpu_ns, result.outside_window_ns, result.common_coverage_ns,
               result.begin_skew_ns, result.end_skew_ns);
    printf("R5_WINDOW_RESULT observation=%s status=incomplete host_budget=unresolved\n",
           ok ? "complete" : "failed");
    return ok ? 2 : 1;
}
#endif

#ifdef BUDGET_TEST
int r5_budget_test_run(void)
{
    return idle();
}

bool r5_budget_test_nanoseconds(clockid_t clock, uint64_t *value)
{
    return nanoseconds(clock, value);
}

int r5_budget_test_phase(bool active, uint64_t duration_ns, uint32_t max_iterations,
                         uint64_t deltas[3], uint32_t *iterations, bool *deadline)
{
    struct clock_phase result;
    int status = clock_phase(active, duration_ns, max_iterations, &result);
    memcpy(deltas, result.delta, sizeof(result.delta));
    *iterations = result.iterations;
    *deadline = result.deadline;
    return status;
}
#else
int main(void)
{
    if (setvbuf(stdout, NULL, _IONBF, 0) != 0) {
        return 1;
    }
    admission();
#ifdef BUDGET_WINDOW
    return characterize_window();
#elif defined(BUDGET_THREADS)
    return characterize_threads();
#elif defined(BUDGET_CLOCK_CHARACTERIZE)
    return characterize();
#elif defined(BUDGET_IDLE)
    return idle();
#else
    return 2;
#endif
}
#endif
