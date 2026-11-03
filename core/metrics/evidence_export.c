#include "internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    sp5_buffer buffer;
    bool valid;
} json_writer;

static void literal(json_writer *writer, const char *text)
{
    if (writer->valid &&
        sp5_buffer_append(&writer->buffer, (sp5_bytes){(const uint8_t *)text, strlen(text)})
                .status != SP5_OK) {
        writer->valid = false;
    }
}

static void number(json_writer *writer, uint64_t value)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%" PRIu64, value);
    if (length < 0 || (size_t)length >= sizeof(text)) {
        writer->valid = false;
        return;
    }
    literal(writer, text);
}

static void optional(json_writer *writer, sp5_optional_u64 value)
{
    if (value.present) {
        number(writer, value.value);
    } else {
        literal(writer, "null");
    }
}

static void nonzero(json_writer *writer, uint64_t value)
{
    optional(writer, (sp5_optional_u64){value != 0, value});
}

static void string(json_writer *writer, const char *value)
{
    if (value == NULL || value[0] == '\0') {
        literal(writer, "null");
        return;
    }
    literal(writer, "\"");
    for (const unsigned char *p = (const unsigned char *)value; *p != 0; ++p) {
        char escaped[8];
        if (*p == '"' || *p == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)*p;
            escaped[2] = '\0';
        } else if (*p < 32) {
            int length = snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)*p);
            if (length < 0 || (size_t)length >= sizeof(escaped)) {
                writer->valid = false;
                return;
            }
        } else {
            escaped[0] = (char)*p;
            escaped[1] = '\0';
        }
        literal(writer, escaped);
    }
    literal(writer, "\"");
}

static void reason(json_writer *writer, sp5_reason value)
{
    string(writer, value == SP5_REASON_NONE ? NULL : sp5_reason_name(value));
}

static void validity(json_writer *writer, sp5_validity value)
{
    string(writer, value == SP5_VALIDITY_VALID     ? "valid"
                   : value == SP5_VALIDITY_INVALID ? "invalid"
                                                   : "unavailable");
}

static void population(json_writer *writer, uint64_t expected, uint64_t represented,
                       uint64_t missing)
{
    literal(writer, "{\"expected\":");
    number(writer, expected);
    literal(writer, ",\"observed\":");
    number(writer, represented);
    literal(writer, ",\"represented\":");
    number(writer, represented);
    literal(writer, ",\"excluded\":0,\"missing\":");
    number(writer, missing);
    literal(writer, ",\"overflow\":0,\"failed_delivery\":0}");
}

static void clock_description(json_writer *writer, const sp5_clock_description *clock)
{
    bool valid = clock->validity == SP5_VALIDITY_VALID && clock->monotonic &&
                 clock->ticks_per_second != 0 && clock->native_resolution_ticks != 0;
    literal(writer, "{\"id\":\"clock-");
    number(writer, clock->domain);
    literal(writer, "\",\"type\":\"monotonic\",\"epoch\":\"epoch-");
    number(writer, clock->epoch);
    literal(writer, "\",\"native_resolution_ns\":");
    if (valid) {
        char resolution[48];
        int length = snprintf(resolution, sizeof(resolution), "%.17g",
                              (double)clock->native_resolution_ticks * 1000000000.0 /
                                  (double)clock->ticks_per_second);
        if (length < 0 || (size_t)length >= sizeof(resolution)) {
            writer->valid = false;
            return;
        }
        literal(writer, resolution);
    } else {
        literal(writer, "null");
    }
    literal(writer, ",\"uncertainty_ns\":");
    optional(writer, clock->verified_uncertainty_ns);
    literal(writer, ",\"uncertainty_reason\":");
    string(writer,
           clock->verified_uncertainty_ns.present ? NULL : "native clock accuracy unverified");
    literal(writer,
            ",\"conversion\":\"floor(native_delta*1000000/ticks_per_second); ticks_per_second=");
    number(writer, clock->ticks_per_second);
    literal(writer, "\",\"quantization_bound_ns\":1000,\"validity\":");
    string(writer, valid ? "valid" : "invalid");
    literal(writer, ",\"reason\":");
    string(writer, valid ? NULL : "invalid native clock");
    literal(writer, "}");
}

static void coverage(json_writer *writer, const sp5_metrics_view *view)
{
    bool gaps = view->disabled_intervals > 0;
    bool invalid = view->coverage_validity == SP5_VALIDITY_INVALID;
    literal(writer, "{\"state\":");
    string(writer, invalid ? "invalid" : (gaps ? "partial" : "complete"));
    literal(writer, ",\"enabled_intervals\":[");
    bool comma = false;
    for (size_t i = 0; i < view->interval_count; ++i) {
        const sp5_metrics_interval *interval = &view->intervals[i];
        if (!interval->enabled) {
            continue;
        }
        if (comma) {
            literal(writer, ",");
        }
        literal(writer, "{\"start\":");
        number(writer, interval->start_us);
        literal(writer, ",\"end\":");
        number(writer, interval->end_us);
        literal(writer, "}");
        comma = true;
    }
    literal(writer, "],\"gaps\":[");
    comma = false;
    for (size_t i = 0; i < view->interval_count; ++i) {
        const sp5_metrics_interval *interval = &view->intervals[i];
        if (interval->enabled || interval->start_us == interval->end_us) {
            continue;
        }
        if (comma) {
            literal(writer, ",");
        }
        literal(writer, "{\"start\":");
        number(writer, interval->start_us);
        literal(writer, ",\"end\":");
        number(writer, interval->end_us);
        literal(writer, ",\"reason\":\"metrics_disabled\"}");
        comma = true;
    }
    literal(writer, "],\"reason\":");
    if (invalid) {
        reason(writer, view->coverage_reason);
    } else {
        string(writer, gaps ? "detailed collection covers enabled intervals only" : NULL);
    }
    literal(writer, "}");
}

typedef struct {
    uint64_t serial;
    uint64_t at;
    bool time_valid;
} export_boundary;

static void envelope(json_writer *writer, const sp5_metrics_view *view, const char *kind,
                     export_boundary boundary)
{
    literal(writer, "{\"schema_version\":\"1.1.0\",\"id\":\"");
    literal(writer, kind);
    literal(writer, "-");
    number(writer, view->session_id);
    literal(writer, "-");
    number(writer, boundary.serial);
    literal(writer, "\",\"session_id\":\"");
    number(writer, view->session_id);
    literal(writer, "\",\"provenance\":");
    string(writer, view->context.synthetic ? "synthetic_fixture" : "console_hardware");
    literal(writer, ",\"source\":\"host_pipeline\",\"kind\":");
    string(writer, kind);
    literal(writer, ",\"clock_domain\":\"clock-");
    number(writer, view->clock.domain);
    literal(writer, "\",\"source_clock\":");
    clock_description(writer, &view->clock);
    literal(writer, ",\"observed_at\":");
    optional(writer, (sp5_optional_u64){boundary.time_valid, boundary.at});
    literal(writer, ",\"population\":");
    population(writer, 1, boundary.time_valid ? 1 : 0, boundary.time_valid ? 0 : 1);
    literal(writer, ",\"coverage\":");
    coverage(writer, view);
    literal(writer, ",\"validity\":");
    string(writer, boundary.time_valid ? "valid" : "invalid");
    literal(writer, ",\"reason\":");
    string(writer, boundary.time_valid ? NULL : "clock or coverage invalid");
    literal(writer, ",\"artifact_refs\":[],\"measurements\":[]");
}

static void quantile(json_writer *writer, const sp5_quantile *q)
{
    literal(writer, "{\"value_us\":");
    optional(writer, q->value_us);
    literal(writer, ",\"lower_bound_us\":");
    optional(writer, q->lower_bound_us);
    literal(writer, ",\"population\":");
    number(writer, q->population);
    literal(writer, ",\"validity\":");
    validity(writer, q->validity);
    literal(writer, ",\"reason\":");
    reason(writer, q->reason);
    literal(writer, "}");
}

static void summary(json_writer *writer, const sp5_metrics_view *view)
{
    static const char *const channels[] = {"capture", "encode",        "packetize",
                                           "send",    "full_pipeline", "input_delivery"};
    literal(writer, "[");
    for (size_t window = 0; window < 2; ++window) {
        for (size_t channel = 0; channel < SP5_STAGE_COUNT; ++channel) {
            const sp5_distribution *d =
                window == 0 ? &view->whole[channel] : &view->rolling[channel];
            const sp5_metrics_population *p =
                window == 0 ? &view->whole_population[channel] : &view->rolling_population[channel];
            if (window != 0 || channel != 0) {
                literal(writer, ",");
            }
            literal(writer, "{\"channel\":");
            string(writer, channels[channel]);
            literal(writer, ",\"window\":");
            string(writer, window == 0 ? "whole_session" : "last_10_seconds");
            literal(writer, ",\"population\":{\"seen\":");
            number(writer, p->seen);
            literal(writer, ",\"represented\":");
            number(writer, p->represented);
            literal(writer, ",\"overflow\":");
            number(writer, p->overflow);
            literal(writer, ",\"missing\":");
            number(writer, p->missing);
            literal(writer, ",\"failed_deliveries\":");
            number(writer, p->failed_deliveries);
            literal(writer, "},\"median\":");
            quantile(writer, &d->median);
            literal(writer, ",\"p95\":");
            quantile(writer, &d->p95);
            literal(writer, ",\"p99\":");
            quantile(writer, &d->p99);
            literal(writer, "}");
        }
    }
    literal(writer, "]");
}

static void retained(json_writer *writer, sp5_optional_u64 first, sp5_optional_u64 last,
                     size_t count)
{
    literal(writer, "{\"first_us\":");
    optional(writer, first);
    literal(writer, ",\"last_us\":");
    optional(writer, last);
    literal(writer, ",\"records\":");
    number(writer, count);
    literal(writer, "}");
}

static void context(json_writer *writer, const sp5_metrics_context *c)
{
    literal(writer, "{\"host_version\":");
    string(writer, c->host_version);
    literal(writer, ",\"firmware_version\":");
    string(writer, c->firmware_version);
    literal(writer, ",\"client_name\":");
    string(writer, c->client_name);
    literal(writer, ",\"codec\":");
    string(writer, c->codec);
    literal(writer, ",\"delivery_boundary\":");
    string(writer, c->delivery_boundary);
    literal(writer, ",\"width\":");
    nonzero(writer, c->mode.width);
    literal(writer, ",\"height\":");
    nonzero(writer, c->mode.height);
    literal(writer, ",\"fps_numerator\":");
    nonzero(writer, c->mode.fps_numerator);
    literal(writer, ",\"fps_denominator\":");
    nonzero(writer, c->mode.fps_denominator);
    literal(writer, ",\"requested_bitrate_kbps\":");
    nonzero(writer, c->requested_bitrate_kbps);
    literal(writer, ",\"chosen_bitrate_kbps\":");
    nonzero(writer, c->mode.chosen_bitrate_kbps);
    literal(writer, ",\"dynamic_range\":");
    string(writer, c->mode.dynamic_range == SP5_SDR             ? "sdr"
                   : c->mode.dynamic_range == SP5_HDR_PQ_BT2020 ? "hdr_pq_bt2020"
                                                                : "unknown");
    literal(writer, "}");
}

static void continuous(json_writer *writer, const sp5_metrics_continuous *c)
{
    literal(writer, "{\"frame_outcomes\":[");
    for (size_t i = 0; i < 5; ++i) {
        if (i != 0) {
            literal(writer, ",");
        }
        number(writer, c->frame_outcomes[i]);
    }
    literal(writer, "],\"encoded_bytes\":");
    number(writer, c->encoded_bytes);
    literal(writer, ",\"network_bytes\":");
    number(writer, c->network_bytes);
    literal(writer, ",\"client_lost_packets\":");
    number(writer, c->client_lost_packets);
    literal(writer, ",\"client_recovery_requests\":");
    number(writer, c->client_recovery_requests);
    literal(writer, ",\"start_us\":");
    number(writer, c->start_us);
    literal(writer, ",\"end_us\":");
    number(writer, c->end_us);
    literal(writer, ",\"encoded_bitrate_bps\":");
    optional(writer, c->encoded_bitrate_bps);
    literal(writer, ",\"network_throughput_bps\":");
    optional(writer, c->network_throughput_bps);
    literal(writer, ",\"frame_rate_millihz\":");
    optional(writer, c->frame_rate_millihz);
    literal(writer, ",\"validity\":");
    validity(writer, c->validity);
    literal(writer, ",\"reason\":");
    reason(writer, c->reason);
    literal(writer, "}");
}

static void snapshot_payload(json_writer *writer, const sp5_metrics_view *view)
{
    literal(writer, "\"context\":");
    context(writer, &view->context);
    literal(writer, ",\"allocated_bytes\":");
    number(writer, view->allocated_bytes);
    literal(writer, ",\"summaries\":");
    summary(writer, view);
    literal(writer, ",\"retained\":{\"frames\":");
    retained(writer, view->first_frame_us, view->last_frame_us, view->retained_frames);
    literal(writer, ",\"inputs\":");
    retained(writer, view->first_input_us, view->last_input_us, view->retained_inputs);
    literal(writer, "},\"continuous\":");
    continuous(writer, &view->continuous);
}

static sp5_result emit(json_writer *writer, const sp5_metrics_sink *sink)
{
    literal(writer, "}\n");
    if (!writer->valid) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    sp5_result result =
        sink->write(sink->context, (sp5_bytes){writer->buffer.data, writer->buffer.size});
    writer->buffer.size = 0;
    return result;
}

static bool clock_valid(const sp5_metrics_view *view)
{
    return view->clock.validity == SP5_VALIDITY_VALID && view->clock.monotonic &&
           view->clock.ticks_per_second != 0 && view->clock.native_resolution_ticks != 0;
}

static sp5_result export_records(sp5_metrics *metrics, json_writer *writer,
                                 const sp5_metrics_sink *sink, bool input, uint64_t begin,
                                 uint64_t end)
{
    const sp5_metrics_view *view = &metrics->export_view;
    for (uint64_t serial = begin; serial < end; ++serial) {
        sp5_frame_record frame = {0};
        sp5_input_record event = {0};
        /* Wait out a bounded worker pass; never truncate the file because of it.
         * The lock is never held across the sink write below. */
        if (pthread_mutex_lock(&metrics->state_lock) != 0) {
            return sp5_failure(SP5_REASON_PLATFORM);
        }
        const sp5_record_ring *ring = input ? &metrics->input_ring : &metrics->frame_ring;
        uint64_t oldest = ring->inserted - ring->count;
        bool available = serial >= oldest && serial < ring->inserted;
        if (available) {
            size_t capacity =
                input ? metrics->capacity.input_records : metrics->capacity.frame_records;
            size_t index = (ring->first + (size_t)(serial - oldest)) % capacity;
            if (input) {
                event = metrics->input_raw[index];
            } else {
                frame = metrics->frame_raw[index];
            }
        }
        if (pthread_mutex_unlock(&metrics->state_lock) != 0) {
            return sp5_failure(SP5_REASON_PLATFORM);
        }
        if (!available) {
            return sp5_failure(SP5_REASON_COVERAGE_GAP);
        }
        uint64_t at = input ? event.observed_us : frame.observed_us;
        uint64_t flags = input ? event.flags : frame.flags;
        bool valid = clock_valid(view) && (flags & SP5_RECORD_CLOCK_INVALID) == 0;
        envelope(writer, view, input ? "input" : "frame", (export_boundary){serial, at, valid});
        if (input) {
            literal(writer, ",\"event_id\":\"");
            number(writer, event.event_id);
            literal(writer, "\",\"controller_id\":");
            number(writer, event.controller_id);
            /* Only receipt is retained as an absolute timestamp. Do not invent a
             * console endpoint by adding two separately rounded observations. */
            literal(writer, ",\"boundary\":\"host_received\",\"authenticated\":");
            literal(writer, (event.flags & SP5_INPUT_AUTHENTICATED) != 0 ? "true" : "false");
        } else {
            literal(writer, ",\"frame_id\":");
            number(writer, frame.frame_id);
            static const char *const boundaries[] = {"captured", "encode_accepted",
                                                     "encode_completed", "network_submitted",
                                                     "host_dropped"};
            unsigned outcome = (unsigned)((frame.flags >> SP5_FRAME_OUTCOME_SHIFT) & 7U);
            literal(writer, ",\"boundary\":");
            string(writer,
                   outcome <= SP5_FRAME_HOST_DROPPED ? boundaries[outcome] : "host_dropped");
            if (outcome >= SP5_FRAME_HOST_DROPPED) {
                literal(writer, ",\"drop_reason\":");
                sp5_reason why = (sp5_reason)((frame.flags >> SP5_RECORD_REASON_SHIFT) & 255U);
                reason(writer, why == SP5_REASON_NONE ? SP5_REASON_STAGE_UNAVAILABLE : why);
            }
        }
        literal(writer, ",\"metrics\":{\"raw\":{\"observed_us\":");
        number(writer, at);
        literal(writer, ",\"durations_us\":[");
        for (size_t i = 0; i < (input ? 1U : 5U); ++i) {
            if (i != 0) {
                literal(writer, ",");
            }
            bool present = valid && (input ? (flags & (SP5_INPUT_VALID | SP5_INPUT_CONFIRMED)) ==
                                                 (SP5_INPUT_VALID | SP5_INPUT_CONFIRMED)
                                           : (flags & (UINT64_C(1) << i)) != 0);
            optional(writer,
                     (sp5_optional_u64){present, input ? event.duration_us : frame.duration_us[i]});
        }
        literal(writer, "],\"flags\":");
        number(writer, flags);
        literal(writer, "}}");
        sp5_result result = emit(writer, sink);
        if (result.status != SP5_OK) {
            return result;
        }
    }
    return sp5_success();
}

sp5_result sp5_metrics_export(sp5_metrics *metrics, const sp5_metrics_sink *sink)
{
    if (metrics == NULL || sink == NULL || sink->write == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (atomic_exchange_explicit(&metrics->exporting, true, memory_order_acquire)) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    /* A second concurrent export is refused above; a worker pass is waited out. */
    if (pthread_mutex_lock(&metrics->state_lock) != 0) {
        atomic_store_explicit(&metrics->exporting, false, memory_order_release);
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    metrics->export_view = metrics->view;
    uint64_t frame_end = metrics->frame_ring.inserted;
    uint64_t frame_begin = frame_end - metrics->frame_ring.count;
    uint64_t input_end = metrics->input_ring.inserted;
    uint64_t input_begin = input_end - metrics->input_ring.count;
    if (pthread_mutex_unlock(&metrics->state_lock) != 0) {
        atomic_store_explicit(&metrics->exporting, false, memory_order_release);
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    json_writer writer = {
        .buffer = {(uint8_t *)metrics->export_buffer, sizeof(metrics->export_buffer), 0},
        .valid = true};
    const sp5_metrics_view *view = &metrics->export_view;
    envelope(
        &writer, view, "metrics_coverage",
        (export_boundary){view->published_us, view->published_us,
                          clock_valid(view) && view->coverage_validity != SP5_VALIDITY_INVALID});
    literal(&writer, ",\"boundary\":");
    string(&writer, view->coverage_validity == SP5_VALIDITY_INVALID ? "coverage_invalidated"
                    : view->metrics_enabled                         ? "metrics_enabled"
                                                                    : "metrics_disabled");
    literal(&writer, ",\"metrics\":{");
    snapshot_payload(&writer, view);
    literal(&writer, "}");
    sp5_result result = emit(&writer, sink);
    if (result.status == SP5_OK) {
        result = export_records(metrics, &writer, sink, false, frame_begin, frame_end);
    }
    if (result.status == SP5_OK) {
        result = export_records(metrics, &writer, sink, true, input_begin, input_end);
    }
    atomic_store_explicit(&metrics->exporting, false, memory_order_release);
    return result;
}

sp5_result sp5_metrics_snapshot_json(const sp5_metrics_view *view, sp5_buffer *output)
{
    if (view == NULL || output == NULL || output->data == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    output->size = 0;
    if (view->interval_count > SP5_METRICS_INTERVAL_CAPACITY ||
        memchr(view->context.host_version, 0, sizeof(view->context.host_version)) == NULL ||
        memchr(view->context.firmware_version, 0, sizeof(view->context.firmware_version)) == NULL ||
        memchr(view->context.client_name, 0, sizeof(view->context.client_name)) == NULL ||
        memchr(view->context.codec, 0, sizeof(view->context.codec)) == NULL ||
        memchr(view->context.delivery_boundary, 0, sizeof(view->context.delivery_boundary)) == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    json_writer w = {.buffer = *output, .valid = true};
    literal(&w, "{\"schema_version\":\"1.0.0\",\"session_id\":\"");
    number(&w, view->session_id);
    literal(&w, "\",\"published_us\":");
    number(&w, view->published_us);
    literal(&w, ",\"metrics_enabled\":");
    literal(&w, view->metrics_enabled ? "true" : "false");
    literal(&w, ",\"synthetic\":");
    literal(&w, view->context.synthetic ? "true" : "false");
    literal(&w, ",\"clock\":");
    clock_description(&w, &view->clock);
    literal(&w, ",\"coverage\":");
    coverage(&w, view);
    literal(&w, ",\"enabled_intervals\":");
    number(&w, view->enabled_intervals);
    literal(&w, ",\"disabled_intervals\":");
    number(&w, view->disabled_intervals);
    literal(&w, ",\"lost_frames\":");
    number(&w, view->lost_frames);
    literal(&w, ",\"lost_inputs\":");
    number(&w, view->lost_inputs);
    literal(&w, ",\"failed_deliveries\":");
    number(&w, view->failed_deliveries);
    literal(&w, ",\"bitrate_adjustment\":");
    string(&w,
           view->context.requested_bitrate_kbps == 0 || view->context.mode.chosen_bitrate_kbps == 0
               ? "unavailable"
           : view->context.requested_bitrate_kbps > view->context.mode.chosen_bitrate_kbps
               ? "lowered_to_maximum"
           : view->context.requested_bitrate_kbps < view->context.mode.chosen_bitrate_kbps
               ? "raised_to_minimum"
               : "none");
    literal(&w, ",\"windows\":{\"whole_session\":{\"start_us\":");
    number(&w, view->continuous.start_us);
    literal(&w, ",\"end_us\":");
    number(&w, view->published_us);
    literal(&w, "},\"last_10_seconds\":{\"start_us\":");
    uint64_t start = view->published_us > SP5_ROLLING_US ? view->published_us - SP5_ROLLING_US : 0;
    if (start < view->continuous.start_us)
        start = view->continuous.start_us;
    number(&w, start);
    literal(&w, ",\"end_us\":");
    number(&w, view->published_us);
    literal(&w, "}},");
    snapshot_payload(&w, view);
    literal(&w, "}");
    if (!w.valid)
        return sp5_failure(SP5_REASON_CAPACITY);
    output->size = w.buffer.size;
    return sp5_success();
}

bool sp5_metrics_enabled(const sp5_metrics *metrics)
{
    return metrics != NULL && atomic_load_explicit(&metrics->enabled, memory_order_acquire);
}
