#ifndef SUNSHINE_CORE_EVIDENCE_H
#define SUNSHINE_CORE_EVIDENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Portable observation contracts, not a console ABI. Authority:
 * specs/001-moonlight-streaming-host/data-model.md (Metrics / Input Evidence)
 * specs/001-moonlight-streaming-host/contracts/measurement-evidence.md.
 * Every span/reference below is borrowed from session-owned bounded storage. */
typedef enum {
    SP5_VALIDITY_UNAVAILABLE = 0,
    SP5_VALIDITY_VALID,
    SP5_VALIDITY_INVALID
} sp5_validity;

typedef enum {
    SP5_REASON_NONE = 0,
    SP5_REASON_UNSUPPORTED,
    SP5_REASON_UNVERIFIED,
    SP5_REASON_INVALID_ARGUMENT,
    SP5_REASON_CAPACITY,
    SP5_REASON_WOULD_BLOCK,
    SP5_REASON_TIMEOUT,
    SP5_REASON_CLOCK,
    SP5_REASON_OVERFLOW,
    SP5_REASON_COVERAGE_GAP,
    SP5_REASON_METRICS_DISABLED,
    SP5_REASON_STAGE_UNAVAILABLE,
    SP5_REASON_PROTECTED,
    SP5_REASON_PROTECTION_UNKNOWN,
    SP5_REASON_SOURCE_CHANGED,
    SP5_REASON_CONFLICT,
    SP5_REASON_DELIVERY_UNCONFIRMED,
    SP5_REASON_PLATFORM,
    SP5_REASON_OWNERSHIP
} sp5_reason;

/* A value of zero is usable only when present. Bounds never become exact values. */
typedef struct {
    bool present;
    uint64_t value;
} sp5_optional_u64;

/* Domain identifies a clock; epoch changes on discontinuity/reset. Subtract only
 * valid observations with matching domain and epoch and established conversion. */
typedef struct {
    uint64_t ticks;
    uint64_t domain;
    uint64_t epoch;
    sp5_validity validity;
    sp5_reason reason;
} sp5_timestamp;

typedef struct {
    uint64_t domain;
    uint64_t epoch;
    uint64_t ticks_per_second;
    uint64_t native_resolution_ticks;
    bool monotonic;
    sp5_optional_u64 verified_uncertainty_ns;
    sp5_validity validity;
    sp5_reason reason;
} sp5_clock_description;

/* Integer microseconds round down with <1 us conversion quantization, separately
 * from clock uncertainty. Reject negative deltas and checked-conversion overflow.
 * These rules do not replace the protocol's distinct 0.1 ms wire rounding. */
typedef struct {
    sp5_optional_u64 value_us;
    sp5_optional_u64 lower_bound_us;
    sp5_optional_u64 upper_bound_us;
    sp5_optional_u64 clock_uncertainty_ns;
    uint32_t quantization_exclusive_upper_bound_ns;
    sp5_validity validity;
    sp5_reason reason;
} sp5_duration;

typedef struct {
    uint64_t expected;
    uint64_t observed;
    uint64_t missing;
    uint64_t excluded;
    uint64_t overflow;
    uint64_t failed_deliveries;
    sp5_timestamp start;
    sp5_timestamp end;
    sp5_validity validity;
    sp5_reason reason;
} sp5_coverage;

typedef enum {
    SP5_STAGE_CAPTURE = 0,
    SP5_STAGE_ENCODE,
    SP5_STAGE_PACKETIZE,
    SP5_STAGE_SEND,
    SP5_STAGE_FULL_PIPELINE,
    SP5_STAGE_INPUT_DELIVERY,
    SP5_STAGE_COUNT
} sp5_stage;

/* Capture: acquisition begins -> usable frame acquired (including wait).
 * Encode: submission begins -> complete hardware bitstream available.
 * Packetize: packet construction begins -> final packet constructed (including FEC).
 * Send: first packet submission begins -> final packet handed to network stack.
 * Full pipeline: capture begins -> final packet handed to network stack.
 * Input: host receipt BEFORE parsing/auth -> documented successful delivery.
 * Core owns packetize/send boundaries; stage p99s never sum to total p99.
 * Detailed spans exist only during metrics-enabled intervals; absent means absent. */
typedef struct {
    sp5_timestamp start;
    sp5_timestamp end;
    sp5_validity validity;
    sp5_reason reason;
} sp5_stage_span;

typedef enum {
    SP5_FRAME_CAPTURED = 0,
    SP5_FRAME_ENCODE_ACCEPTED,
    SP5_FRAME_ENCODE_COMPLETED,
    SP5_FRAME_NETWORK_SUBMITTED,
    SP5_FRAME_HOST_DROPPED
} sp5_frame_outcome;

typedef struct {
    uint64_t session_id;
    uint64_t frame_id;
    uint64_t source_epoch;
    sp5_stage_span stages[SP5_STAGE_COUNT];
    sp5_frame_outcome outcome;
    sp5_reason reason;
} sp5_frame_evidence;

/* This independent observation remains available with metrics off. Capture stamp
 * -> pre-header construction, before FEC/transmission; zero wire units indicate
 * unavailable/repeated, NOT zero latency. See protocol-notes.md section 6 for
 * (us + 50) / 100 saturation. It cannot contain the future final-send time. */
typedef struct {
    uint64_t session_id;
    uint64_t frame_id;
    sp5_timestamp capture_timestamp;
    sp5_timestamp header_measurement;
    uint16_t wire_tenths_ms;
    bool repeated;
    sp5_validity validity;
    sp5_reason reason;
} sp5_protocol_latency_evidence;

typedef enum {
    SP5_INPUT_DELIVERY_UNCONFIRMED = 0,
    SP5_INPUT_DELIVERY_CONFIRMED,
    SP5_INPUT_DELIVERY_FAILED
} sp5_input_delivery_status;

/* Client-send/ACK are diagnostics in their own domains. Only confirmed delivery
 * with a sourced boundary and matching host clock can supply an SC-005 endpoint.
 * Expected visible console response remains a separate functional observation. */
typedef struct {
    uint64_t session_id;
    uint64_t event_id;
    uint32_t controller_id;
    sp5_timestamp client_sent;
    sp5_timestamp transport_ack;
    sp5_timestamp host_received;
    sp5_timestamp console_delivered;
    sp5_input_delivery_status delivery;
    sp5_reason reason;
    const char *delivery_boundary_ref;
    /* Set immediately before a platform deliver call. Arrival/removal packets
     * are authenticated operations, but are not failed delivery observations. */
    bool delivery_attempted;
} sp5_input_evidence;

#endif
