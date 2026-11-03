/* Deterministic T044 diagnostic failures; no console or real sleep. */
#include "budget_test.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            if (fputs("budget probe test: " #condition "\n", stderr) == EOF)                       \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static clockid_t reversed_clock;
static clockid_t unavailable_clock;
static unsigned reads[3], waits;
static bool wait_failure, interrupted;
static bool phase_clock, fixed_time;
static struct timespec supplied_time;
static uint64_t clock_step;
static unsigned unavailable_after;

int r5_budget_test_clock(clockid_t clock, struct timespec *value)
{
    unsigned slot = clock == CLOCK_MONOTONIC ? 0 : (clock == CLOCK_THREAD_CPUTIME_ID ? 1 : 2);
    if (clock == unavailable_clock && reads[slot] >= unavailable_after) {
        return -1;
    }
    if (fixed_time) {
        *value = supplied_time;
        return 0;
    }
    if (phase_clock) {
        uint64_t ns = UINT64_C(1000000000) + reads[slot]++ * clock_step;
        if (clock == reversed_clock && reads[slot] > 1)
            ns = 0;
        value->tv_sec = (time_t)(ns / UINT64_C(1000000000));
        value->tv_nsec = (long)(ns % UINT64_C(1000000000));
        return 0;
    }
    bool ending = reads[slot]++ != 0;
    value->tv_sec = ending && clock == reversed_clock ? 0 : (ending ? 11 : 1);
    value->tv_nsec = 0;
    return 0;
}

int r5_budget_test_sleep(const struct timespec *delay, struct timespec *remaining)
{
    if (delay->tv_sec < 0 || (delay->tv_sec == 0 && delay->tv_nsec <= 0)) {
        errno = EINVAL;
        return -1;
    }
    if (wait_failure) {
        errno = EINVAL;
        return -1;
    }
    if (interrupted && waits++ == 0) {
        if (remaining != NULL) {
            *remaining = (struct timespec){1, 0};
        }
        errno = EINTR;
        return -1;
    }
    return 0;
}

static void reset(void)
{
    reversed_clock = CLOCK_REALTIME;
    unavailable_clock = CLOCK_REALTIME;
    memset(reads, 0, sizeof(reads));
    waits = 0;
    wait_failure = false;
    interrupted = false;
    phase_clock = false;
    fixed_time = false;
    clock_step = 100;
    unavailable_after = 0;
}

struct thread_fixture {
    unsigned creates, joins, fail_create, fail_join;
    struct r5_thread_worker *workers[2];
    bool missing;
};

static struct r5_thread_sample valid_sample(void)
{
    return (struct r5_thread_sample){
        .begin = {100, 50, 200}, .end = {300000200, 100050, 300000300}, .complete = true};
}

static int fake_create(void *context, unsigned index, struct r5_thread_worker *worker)
{
    struct thread_fixture *f = context;
    ++f->creates;
    if (f->fail_create == index + 1)
        return 1;
    f->workers[index] = worker;
    return 0;
}

static int fake_join(void *context, unsigned index)
{
    struct thread_fixture *f = context;
    ++f->joins;
    if (!atomic_load_explicit(f->workers[index]->start, memory_order_acquire))
        return 1;
    f->workers[index]->sample = valid_sample();
    f->workers[index]->sample.complete = !f->missing;
    return f->fail_join == index + 1 ? 1 : 0;
}

static int thread_tests(void)
{
    for (unsigned mode = 0; mode < 6; ++mode) {
        struct thread_fixture f = {0};
        if (mode == 1 || mode == 2)
            f.fail_create = mode;
        if (mode == 3 || mode == 4)
            f.fail_join = mode - 2;
        f.missing = mode == 5;
        const struct r5_thread_api api = {fake_create, fake_join, &f};
        struct r5_thread_run run;
        uint64_t sum = 123;
        CHECK(r5_thread_run(&api, &run, false, &sum) == (mode == 0));
        CHECK(f.joins == (mode == 1 ? 0U : mode == 2 ? 1U : 2U));
        CHECK(sum == (mode == 0 ? 200000U : 123U));
        CHECK(!run.workers[0].active);
    }
    struct thread_fixture f = {0};
    const struct r5_thread_api api = {fake_create, fake_join, &f};
    struct r5_thread_run run;
    uint64_t sum;
    CHECK(r5_thread_run(&api, &run, true, &sum));
    CHECK(run.workers[0].active && !run.workers[1].active);
    for (unsigned mode = 0; mode < 7; ++mode) {
        struct r5_thread_sample samples[2] = {valid_sample(), valid_sample()};
        if (mode == 0)
            samples[1].complete = false;
        if (mode == 1)
            samples[1].end[1] = 0;
        if (mode == 2)
            samples[1].end[0] = 0;
        if (mode == 3)
            samples[1].begin[2] = 0;
        if (mode == 4)
            samples[1].end[2] += UINT64_C(5000001);
        if (mode == 5) {
            samples[1].begin[0] += UINT64_C(100000000);
            samples[1].begin[2] += UINT64_C(100000000);
            samples[1].end[0] += UINT64_C(100000000);
            samples[1].end[2] += UINT64_C(100000000);
        }
        if (mode == 6) {
            samples[0].begin[1] = samples[1].begin[1] = 0;
            samples[0].end[1] = samples[1].end[1] = UINT64_MAX;
        }
        sum = 123;
        CHECK(!r5_thread_totals(samples, &sum) && sum == 123);
    }
    for (unsigned mode = 0; mode < 5; ++mode) {
        reset();
        phase_clock = true;
        clock_step = UINT64_C(100000000);
        atomic_bool start;
        atomic_init(&start, true);
        struct r5_thread_worker worker = {.start = &start};
        if (mode == 1)
            unavailable_clock = CLOCK_THREAD_CPUTIME_ID;
        if (mode == 2)
            reversed_clock = CLOCK_MONOTONIC;
        if (mode == 3)
            wait_failure = true;
        if (mode == 4)
            clock_step = 0;
        r5_budget_test_worker(&worker);
        CHECK(worker.sample.complete == (mode == 0));
    }
    puts("PASS two-thread create/join failures, missing completion, role swap, reversed clocks, "
         "bracket uncertainty, interval mismatch, sum overflow and worker faults");
    return 0;
}

static int window_totals_tests(void)
{
    const uint64_t begin = UINT64_C(1000000000), end = begin + R5_WINDOW_NS;
    struct r5_thread_sample samples[2] = {{.begin = {begin - 200, 50, begin - 100},
                                           .end = {end + 100, 150, end + 200},
                                           .complete = true},
                                          {.begin = {begin - 300, 70, begin - 150},
                                           .end = {end + 150, 270, end + 300},
                                           .complete = true}};
    struct r5_window_result result = {0};
    CHECK(r5_window_totals(samples, begin, end, &result));
    CHECK(result.enclosing_cpu_ns == 300 && result.outside_window_ns == 1000);
    CHECK(result.common_coverage_ns == R5_WINDOW_NS + 200);
    CHECK(result.begin_skew_ns == 200 && result.end_skew_ns == 200);
    for (unsigned mode = 0; mode < 10; ++mode) {
        struct r5_thread_sample broken[2] = {samples[0], samples[1]};
        if (mode == 0)
            broken[1].complete = false;
        if (mode == 1)
            broken[1].begin[2] = begin + 1;
        if (mode == 2)
            broken[1].end[0] = end - 1;
        if (mode == 3)
            broken[1].begin[0] = begin - R5_WINDOW_MARGIN_NS - 1;
        if (mode == 4)
            broken[1].end[2] = end + R5_WINDOW_MARGIN_NS + 1;
        if (mode == 5)
            broken[1].end[1] = 0;
        if (mode == 6)
            broken[1].begin[2] = broken[1].begin[0] - 1;
        if (mode == 7)
            broken[1].end[2] = broken[1].end[0] - 1;
        if (mode == 8)
            broken[1].end[1] = UINT64_MAX;
        result = (struct r5_window_result){.enclosing_cpu_ns = 123};
        CHECK(!r5_window_totals(broken, begin, mode == 9 ? begin - 1 : end, &result));
        CHECK(result.enclosing_cpu_ns == 123 && result.common_coverage_ns == 0);
    }
    CHECK(!r5_window_totals(samples, begin, end - 1, &result));
    puts("PASS window enclosure, coverage/skew, missing/late samples, clock reversal, overflow and "
         "unchanged rejected totals");
    return 0;
}

struct window_fixture {
    struct r5_window_worker *workers[2];
    uint64_t now;
    unsigned creates, joins, polls, now_calls, ready_after;
    unsigned fail_create, fail_join;
    bool no_ready, no_done, missing, frozen, reverse, overflow, wait_error, late, unavailable;
};

static int fake_window_create(void *context, unsigned index, struct r5_window_worker *worker)
{
    struct window_fixture *f = context;
    ++f->creates;
    if (f->fail_create == index + 1)
        return 1;
    f->workers[index] = worker;
    return 0;
}

static int fake_window_join(void *context, unsigned index)
{
    struct window_fixture *f = context;
    ++f->joins;
    if (!atomic_load_explicit(&f->workers[index]->done, memory_order_acquire))
        return 1;
    return f->fail_join == index + 1 ? 1 : 0;
}

static bool fake_window_now(void *context, uint64_t *now)
{
    struct window_fixture *f = context;
    ++f->now_calls;
    if (f->unavailable && f->now_calls == 4)
        return false;
    if (!f->frozen)
        f->now += UINT64_C(1000000);
    *now = f->overflow ? UINT64_MAX - UINT64_C(1000000) : f->now;
    if (f->reverse && f->now_calls == 4)
        *now = 0;
    return true;
}

static bool fake_window_wait(void *context)
{
    struct window_fixture *f = context;
    ++f->polls;
    for (unsigned i = 0; i < 2; ++i) {
        struct r5_window_worker *w = f->workers[i];
        if (!w)
            continue;
        if (!f->no_ready && f->polls >= f->ready_after)
            atomic_store_explicit(&w->ready, true, memory_order_release);
        if (atomic_load_explicit(&w->run->published, memory_order_acquire) && !f->no_done) {
            uint64_t begin = w->run->begin_ns, end = w->run->end_ns;
            if (begin != 0) {
                w->sample = (struct r5_thread_sample){.begin = {begin - 200, 10, begin - 100},
                                                      .end = {end + 100, 1010, end + 200},
                                                      .complete = !f->missing};
                if (f->late)
                    w->sample.end[2] = end + R5_WINDOW_MARGIN_NS + 1;
            }
            atomic_store_explicit(&w->done, true, memory_order_release);
        }
    }
    return !f->wait_error;
}

static int window_run_tests(void)
{
    for (unsigned mode = 0; mode < 15; ++mode) {
        struct window_fixture f = {.now = UINT64_C(1000000000), .ready_after = 3};
        if (mode == 1 || mode == 2)
            f.fail_create = mode;
        if (mode == 3 || mode == 4)
            f.fail_join = mode - 2;
        if (mode == 5)
            f.no_ready = true;
        if (mode == 6)
            f.no_done = true;
        if (mode == 7)
            f.missing = true;
        if (mode == 8)
            f.frozen = f.no_ready = true;
        if (mode == 9)
            f.reverse = true;
        if (mode == 10)
            f.overflow = true;
        if (mode == 11)
            f.wait_error = true;
        if (mode == 12)
            f.late = true;
        if (mode == 13)
            f.frozen = f.no_done = true;
        if (mode == 14)
            f.unavailable = true;
        const struct r5_window_api api = {fake_window_create, fake_window_join, fake_window_now,
                                          fake_window_wait, &f};
        struct r5_window_run run;
        struct r5_window_result result = {.enclosing_cpu_ns = 123};
        CHECK(r5_window_run(&api, &run, &result) == (mode == 0));
        CHECK(result.enclosing_cpu_ns == (mode == 0 ? 2000U : 123U));
        CHECK(f.polls <= 24000);
        CHECK(f.joins == (mode == 1 || mode == 6 || mode == 13 ? 0U : mode == 2 ? 1U : 2U));
        if (mode == 0) {
            CHECK(f.polls >= 4 && run.end_ns - run.begin_ns == R5_WINDOW_NS);
            CHECK(run.workers[1].active && !run.workers[0].active);
        }
    }
    puts("PASS coordinated delayed readiness, create/join faults, no-ready/no-done caps, "
         "frozen/reversed clocks, overflow, wait faults and late completion");
    for (unsigned mode = 0; mode < 10; ++mode) {
        reset();
        phase_clock = true;
        clock_step = UINT64_C(100000);
        struct r5_window_run run = {.begin_ns = UINT64_C(1050000000),
                                    .end_ns = UINT64_C(1050000000) + R5_WINDOW_NS};
        atomic_init(&run.published, mode != 7);
        atomic_init(&run.cancel, mode == 6);
        struct r5_window_worker worker = {.run = &run, .active = mode == 9};
        atomic_init(&worker.ready, false);
        atomic_init(&worker.done, false);
        if (mode == 1)
            unavailable_clock = CLOCK_THREAD_CPUTIME_ID;
        if (mode == 2)
            reversed_clock = CLOCK_MONOTONIC;
        if (mode == 3)
            wait_failure = true;
        if (mode == 4)
            clock_step = 0;
        if (mode == 5) {
            run.begin_ns = UINT64_C(1000000000);
            run.end_ns = run.begin_ns + R5_WINDOW_NS;
        }
        if (mode == 8) {
            unavailable_clock = CLOCK_THREAD_CPUTIME_ID;
            unavailable_after = 1;
        }
        /* Advance the fake clock by 1ms so the ten-second sleep loop remains within its cap. */
        if (mode == 0 || mode == 1 || mode == 8 || mode == 9)
            clock_step = UINT64_C(1000000);
        r5_budget_test_window_worker(&worker);
        CHECK(atomic_load_explicit(&worker.done, memory_order_acquire));
        CHECK(worker.sample.complete == (mode == 0 || mode == 9));
    }
    puts("PASS window worker snapshots, missed start, cancellation, publication/clock caps, "
         "unavailable/reversed clocks and wait failure");
    return 0;
}

int main(void)
{
    CHECK(window_totals_tests() == 0);
    CHECK(window_run_tests() == 0);
    CHECK(thread_tests() == 0);
    reset();
    CHECK(r5_budget_test_run() == 2);
    puts("PASS valid clocks remain Incomplete");
    const clockid_t clocks[] = {CLOCK_MONOTONIC, CLOCK_THREAD_CPUTIME_ID, CLOCK_PROCESS_CPUTIME_ID};
    for (size_t i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i) {
        reset();
        reversed_clock = clocks[i];
        CHECK(r5_budget_test_run() == 1);
        printf("PASS reversed clock index=%zu fails\n", i);
        reset();
        unavailable_clock = clocks[i];
        CHECK(r5_budget_test_run() == 1);
        printf("PASS unavailable clock index=%zu fails\n", i);
    }
    reset();
    wait_failure = true;
    CHECK(r5_budget_test_run() == 1);
    puts("PASS failed wait fails");
    reset();
    interrupted = true;
    CHECK(r5_budget_test_run() == 2 && waits == 2);
    puts("PASS interrupted wait resumes and remains Incomplete");
    uint64_t deltas[3];
    uint32_t iterations;
    bool deadline;
    for (unsigned active = 0; active < 2; ++active) {
        reset();
        phase_clock = true;
        CHECK(r5_budget_test_phase(active != 0, 300, 10, deltas, &iterations, &deadline) == 2);
        CHECK(deadline && iterations == 2 && deltas[0] == 400 && deltas[1] == 100 &&
              deltas[2] == 100);
        reset();
        phase_clock = true;
        clock_step = 0;
        CHECK(r5_budget_test_phase(active != 0, 300, 3, deltas, &iterations, &deadline) == 1);
        CHECK(!deadline && iterations == 3 && deltas[0] == 0);
        for (size_t i = 0; i < 3; ++i) {
            reset();
            phase_clock = true;
            unavailable_clock = clocks[i];
            CHECK(r5_budget_test_phase(active != 0, 300, 10, deltas, &iterations, &deadline) == 1);
            reset();
            phase_clock = true;
            unavailable_clock = clocks[i];
            unavailable_after = i == 0 ? 4 : 1;
            CHECK(r5_budget_test_phase(active != 0, 300, 10, deltas, &iterations, &deadline) == 1);
            reset();
            phase_clock = true;
            reversed_clock = clocks[i];
            CHECK(r5_budget_test_phase(active != 0, 300, 10, deltas, &iterations, &deadline) == 1);
        }
    }
    puts("PASS sleep/active deadline, iteration cap, unavailable and reversed clocks");
    reset();
    phase_clock = true;
    wait_failure = true;
    CHECK(r5_budget_test_phase(false, 300, 10, deltas, &iterations, &deadline) == 1);
    reset();
    phase_clock = true;
    interrupted = true;
    CHECK(r5_budget_test_phase(false, 300, 10, deltas, &iterations, &deadline) == 2);
    reset();
    phase_clock = true;
    CHECK(r5_budget_test_phase(true, 0, 10, deltas, &iterations, &deadline) == 1);
    CHECK(r5_budget_test_phase(true, 300, 0, deltas, &iterations, &deadline) == 1);
    CHECK(r5_budget_test_phase(true, UINT64_MAX, 3, deltas, &iterations, &deadline) == 1);
    CHECK(iterations == 3 && !deadline);
    puts("PASS wait faults, invalid bounds, overflow-free duration comparison");
    reset();
    fixed_time = true;
    uint64_t ns = 123;
    const struct timespec invalid[] = {{-1, 0},
                                       {0, -1},
                                       {0, 1000000000L},
                                       {(time_t)(UINT64_MAX / UINT64_C(1000000000) + 1), 0},
                                       {(time_t)(UINT64_MAX / UINT64_C(1000000000)), 999999999L}};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        supplied_time = invalid[i];
        CHECK(!r5_budget_test_nanoseconds(CLOCK_MONOTONIC, &ns) && ns == 123);
    }
    supplied_time = (struct timespec){(time_t)(UINT64_MAX / UINT64_C(1000000000)),
                                      (long)(UINT64_MAX % UINT64_C(1000000000))};
    CHECK(r5_budget_test_nanoseconds(CLOCK_MONOTONIC, &ns) && ns == UINT64_MAX);
    puts("PASS invalid timespec and exact UINT64_MAX conversion boundary");
    return 0;
}
