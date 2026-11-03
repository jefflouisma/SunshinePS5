#ifndef SUNSHINE_CORE_METRICS_H
#define SUNSHINE_CORE_METRICS_H

#include "protocol.h"

/* Portable metrics contract; layouts/limits from data-model.md, Metrics Capture.
 * Observation timestamps are integer microseconds in the declared session clock.
 * Frame flags bits 0..4 mark valid stages; input flags mark valid/confirmed.
 * Record IDs never authorize a session. No cross-session aggregation. */
typedef struct {
    uint64_t frame_id;
    uint64_t observed_us;
    uint64_t duration_us[5];
    uint64_t flags;
} sp5_frame_record;
typedef struct {
    uint64_t event_id;
    uint64_t observed_us;
    uint64_t duration_us;
    uint32_t controller_id;
    uint32_t flags;
} sp5_input_record;
enum {
    SP5_FRAME_ALL_VALID = 31,
    SP5_INPUT_VALID = 1,
    SP5_INPUT_CONFIRMED = 2,
    SP5_HISTOGRAM_BINS = 65536
};

typedef struct {
    uint64_t bins[SP5_HISTOGRAM_BINS];
    uint64_t count;
    uint64_t overflow;
    uint64_t missing;
    uint64_t failed_deliveries;
    sp5_validity validity;
    sp5_reason reason;
} sp5_histogram;
typedef struct {
    sp5_optional_u64 value_us;
    sp5_optional_u64 lower_bound_us;
    uint64_t population;
    sp5_validity validity;
    sp5_reason reason;
} sp5_quantile;
SP5_CORE_API void sp5_histogram_init(sp5_histogram *histogram);
SP5_CORE_API sp5_result sp5_histogram_add(sp5_histogram *histogram, uint64_t value_us);
SP5_CORE_API sp5_quantile sp5_histogram_percentile(const sp5_histogram *histogram,
                                                   unsigned percentile);
/* Strict exact frame interval, never a rounded 16.7ms label. */
SP5_CORE_API bool sp5_latency_below_frame_interval(uint64_t value_us, uint32_t fps_numerator,
                                                   uint32_t fps_denominator);

typedef struct sp5_metrics sp5_metrics;
typedef struct {
    size_t frame_records;
    size_t input_records;
    size_t queue_records; /* Separate frame and input queues, each this bound. */
} sp5_metrics_capacity;
typedef struct {
    sp5_quantile median;
    sp5_quantile p95;
    sp5_quantile p99;
} sp5_distribution;
typedef struct {
    uint64_t seen;
    uint64_t represented;
    uint64_t overflow;
    uint64_t missing;
    uint64_t failed_deliveries;
} sp5_metrics_population;
typedef struct {
    uint64_t start_us;
    uint64_t end_us;
    bool enabled;
} sp5_metrics_interval;
typedef struct {
    char host_version[64];
    char firmware_version[64];
    char client_name[128];
    char codec[32];
    char delivery_boundary[256];
    sp5_media_config mode;
    uint32_t requested_bitrate_kbps;
    /* False requires actual console provenance; never inferred from success. */
    bool synthetic;
} sp5_metrics_context;
typedef struct {
    uint64_t frame_outcomes[5];
    uint64_t encoded_bytes;
    uint64_t network_bytes;
    uint64_t client_lost_packets;
    uint64_t client_recovery_requests;
    uint64_t start_us;
    uint64_t end_us;
    sp5_optional_u64 encoded_bitrate_bps;
    sp5_optional_u64 network_throughput_bps;
    sp5_optional_u64 frame_rate_millihz;
    sp5_validity validity;
    sp5_reason reason;
} sp5_metrics_continuous;
typedef struct {
    uint64_t session_id;
    uint64_t published_us;
    sp5_distribution whole[SP5_STAGE_COUNT];
    sp5_distribution rolling[SP5_STAGE_COUNT];
    size_t retained_frames;
    size_t retained_inputs;
    sp5_optional_u64 first_frame_us;
    sp5_optional_u64 last_frame_us;
    sp5_optional_u64 first_input_us;
    sp5_optional_u64 last_input_us;
    uint64_t lost_frames;
    uint64_t lost_inputs;
    uint64_t failed_deliveries;
    uint64_t enabled_intervals;
    uint64_t disabled_intervals;
    bool metrics_enabled;
    sp5_validity coverage_validity;
    sp5_reason coverage_reason;
    size_t allocated_bytes;
    sp5_clock_description clock;
    sp5_metrics_population whole_population[SP5_STAGE_COUNT];
    sp5_metrics_population rolling_population[SP5_STAGE_COUNT];
    sp5_metrics_interval intervals[128];
    size_t interval_count;
    sp5_metrics_context context;
    sp5_metrics_continuous continuous;
} sp5_metrics_view;
typedef struct {
    void *context;
    sp5_result (*write)(void *context, sp5_bytes chunk);
} sp5_metrics_sink;
typedef struct {
    void *context;
    void (*published)(void *context, const sp5_metrics_view *snapshot);
} sp5_metrics_observer;
/* Allocation only at setup; caller may choose smaller test capacities. Production
 * maxima: 131072 frame/2097152 input records; all state fits 83886080 bytes.
 * Worker owns histograms; producers never aggregate, allocate, export, or wait.
 * Window is (now-10s,now]; raw expiry is age >600s. Queue loss invalidates
 * affected summaries; raw capacity loss invalidates rolling if expiry is lost.
 * Observer/export consumers may stall without delaying either producer. */
SP5_CORE_API sp5_metrics *sp5_metrics_create(uint64_t session_id, sp5_metrics_capacity capacity,
                                             const sp5_clock_ops *clock);
SP5_CORE_API void sp5_metrics_destroy(sp5_metrics *metrics);
SP5_CORE_API sp5_result sp5_metrics_enable(sp5_metrics *metrics, bool enabled, uint64_t now_us);
SP5_CORE_API sp5_result sp5_metrics_stamp(sp5_metrics *metrics, sp5_timestamp *timestamp);
SP5_CORE_API sp5_result sp5_metrics_protocol_header(sp5_metrics *metrics, uint64_t frame_id,
                                                    sp5_timestamp capture, bool repeated,
                                                    sp5_protocol_latency_evidence *evidence);
SP5_CORE_API sp5_result sp5_metrics_push_frame(sp5_metrics *metrics,
                                               const sp5_frame_record *record);
SP5_CORE_API sp5_result sp5_metrics_push_input(sp5_metrics *metrics,
                                               const sp5_input_record *record);
SP5_CORE_API sp5_result sp5_metrics_worker_step(sp5_metrics *metrics, uint64_t now_us,
                                                const sp5_metrics_observer *observer);
/* Management readers: an in-progress worker pass (bounded, never waiting on
 * anything) is waited out rather than reported as a failed view or truncated
 * export. Producers and the worker never wait for these readers. */
SP5_CORE_API sp5_result sp5_metrics_snapshot(const sp5_metrics *metrics, sp5_metrics_view *view);
SP5_CORE_API sp5_result sp5_metrics_export(sp5_metrics *metrics, const sp5_metrics_sink *sink);

/* Reusable bounded snapshot JSON for management readers. No engine mutation,
 * clock reads or heap allocation; capacity failure resets output size to zero.
 * The view must be an immutable snapshot from sp5_metrics_snapshot(). */
/* Atomic current collection state, no allocation or clock read. */
bool sp5_metrics_enabled(const sp5_metrics *metrics);
sp5_result sp5_metrics_snapshot_json(const sp5_metrics_view *view, sp5_buffer *output);

/* Context is copied once before producers start. mode.codec is ignored; the
 * bounded codec array is authoritative. Continuous counters require no clock
 * reads and remain available with detailed metrics off. One owner per path. */
sp5_result sp5_metrics_set_context(sp5_metrics *metrics, const sp5_metrics_context *context);
typedef struct {
    sp5_frame_outcome outcome;
    uint64_t encoded_bytes;
    uint64_t network_bytes;
} sp5_metrics_stream_event;
sp5_result sp5_metrics_count_stream(sp5_metrics *metrics, sp5_metrics_stream_event event);
sp5_result sp5_metrics_count_client(sp5_metrics *metrics, uint64_t lost_packets,
                                    uint64_t recovery_requests);

#endif
