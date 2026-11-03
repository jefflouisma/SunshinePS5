#ifndef R5_BUDGET_TEST_H
#define R5_BUDGET_TEST_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Internal diagnostic seam; samples bracket thread CPU with monotonic reads. */
struct r5_thread_sample {
    uint64_t begin[3], end[3];
    bool complete;
};
struct r5_thread_worker {
    atomic_bool *start;
    bool active;
    struct r5_thread_sample sample;
};
struct r5_thread_run {
    atomic_bool start;
    struct r5_thread_worker workers[2];
    bool created[2], joined[2];
};
struct r5_thread_api {
    int (*create)(void *, unsigned, struct r5_thread_worker *);
    int (*join)(void *, unsigned);
    void *context;
};
bool r5_thread_totals(const struct r5_thread_sample samples[2], uint64_t *sum);
bool r5_thread_run(const struct r5_thread_api *, struct r5_thread_run *, bool swap, uint64_t *sum);
void r5_budget_test_worker(struct r5_thread_worker *);

#define R5_WINDOW_NS UINT64_C(10000000000)
#define R5_WINDOW_MARGIN_NS UINT64_C(5000000)
struct r5_window_run;
struct r5_window_worker {
    struct r5_window_run *run;
    atomic_bool ready, done;
    bool active;
    struct r5_thread_sample sample;
};
struct r5_window_run {
    atomic_bool published, cancel;
    uint64_t begin_ns, end_ns;
    uint64_t coordinator_last_ns;
    bool coordinator_sampled;
    struct r5_window_worker workers[2];
    bool created[2], joined[2];
};
struct r5_window_result {
    uint64_t enclosing_cpu_ns, outside_window_ns, common_coverage_ns;
    uint64_t begin_skew_ns, end_skew_ns;
};
struct r5_window_api {
    int (*create)(void *, unsigned, struct r5_window_worker *);
    int (*join)(void *, unsigned);
    bool (*now)(void *, uint64_t *);
    bool (*wait)(void *);
    void *context;
};
bool r5_window_totals(const struct r5_thread_sample samples[2], uint64_t begin_ns, uint64_t end_ns,
                      struct r5_window_result *result);
bool r5_window_run(const struct r5_window_api *, struct r5_window_run *, struct r5_window_result *);
void r5_budget_test_window_worker(struct r5_window_worker *);

/* Test-only substitutions; ordinary payload builds use the SDK POSIX calls. */
int r5_budget_test_clock(clockid_t clock, struct timespec *value);
int r5_budget_test_sleep(const struct timespec *delay, struct timespec *remaining);
int r5_budget_test_run(void);
bool r5_budget_test_nanoseconds(clockid_t clock, uint64_t *value);
int r5_budget_test_phase(bool active, uint64_t duration_ns, uint32_t max_iterations,
                         uint64_t deltas[3], uint32_t *iterations, bool *deadline);

#endif
