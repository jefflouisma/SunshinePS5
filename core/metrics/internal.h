#ifndef SUNSHINE_METRICS_INTERNAL_H
#define SUNSHINE_METRICS_INTERNAL_H

#include "records.h"
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>

enum { SP5_METRICS_BYTES = 83886080 };
#define SP5_ROLLING_US UINT64_C(10000000)
#define SP5_RETENTION_US UINT64_C(600000000)

typedef struct {
    atomic_size_t head;
    atomic_size_t tail;
    atomic_uint_fast64_t lost;
    atomic_bool overflow;
    void *records;
} sp5_record_queue;
typedef struct {
    size_t first;
    size_t count;
    size_t rolling_expired;
    uint64_t inserted;
    sp5_optional_u64 latest;
} sp5_record_ring;

struct sp5_metrics {
    sp5_histogram whole[SP5_STAGE_COUNT];
    sp5_histogram rolling[SP5_STAGE_COUNT];
    sp5_metrics_capacity capacity;
    sp5_clock_ops clock;
    sp5_clock_description description;
    uint64_t session_id;
    size_t allocated_bytes;
    atomic_bool enabled;
    sp5_record_queue frames;
    sp5_record_queue inputs;
    sp5_frame_record *frame_raw;
    sp5_input_record *input_raw;
    sp5_record_ring frame_ring;
    sp5_record_ring input_ring;
    pthread_mutex_t state_lock;
    sp5_metrics_view view;
    sp5_metrics_interval intervals[SP5_METRICS_INTERVAL_CAPACITY];
    size_t interval_count;
    uint64_t enabled_intervals;
    uint64_t disabled_intervals;
    uint64_t transition_us;
    uint64_t worker_us;
    uint64_t session_start_us;
    bool worker_started;
    sp5_reason coverage_reason;
    uint64_t failed_deliveries;
    uint64_t frame_losses_applied;
    uint64_t input_losses_applied;
    uint64_t rolling_invalid_until[SP5_STAGE_COUNT];
    uint64_t rolling_gap_missing[SP5_STAGE_COUNT];
    bool rolling_permanent[SP5_STAGE_COUNT];
    sp5_metrics_context context;
    atomic_uint_fast64_t continuous[9];
    atomic_bool continuous_overflow;
    atomic_uint_fast64_t continuous_sequence[2];
    atomic_bool exporting;
    sp5_metrics_view export_view;
    char export_buffer[65536];
};

void sp5_metrics_invalidate(sp5_metrics *metrics, sp5_reason reason);
bool sp5_metrics_count(sp5_metrics *metrics, uint64_t *counter, uint64_t increment);
void sp5_metrics_expire(sp5_metrics *metrics, uint64_t now_us);
void sp5_metrics_retain_frame(sp5_metrics *metrics, const sp5_frame_record *record, uint64_t now);
void sp5_metrics_retain_input(sp5_metrics *metrics, const sp5_input_record *record, uint64_t now);
void sp5_metrics_aggregate_frame(sp5_metrics *metrics, const sp5_frame_record *record, bool rolling,
                                 bool remove);
void sp5_metrics_aggregate_input(sp5_metrics *metrics, const sp5_input_record *record, bool rolling,
                                 bool remove);
void sp5_metrics_publish(sp5_metrics *metrics, uint64_t now);

#endif
