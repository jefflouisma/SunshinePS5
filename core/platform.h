#ifndef SUNSHINE_CORE_PLATFORM_H
#define SUNSHINE_CORE_PLATFORM_H

#include "evidence.h"

/* Proposed portable adapter contracts only: no SDK types, symbols or callable ABI
 * are implied. Implementations need sourced interfaces and firmware verification
 * under research R3/T040-T045. Fakes MUST identify themselves as synthetic.
 * All contexts and storage are session-owned/preallocated; successful open calls
 * acquire resources, failed opens unwind their own partial work. At termination,
 * stop input admission and release_all FIRST, then quiesce producers, return leases,
 * close successful opens in reverse order and rollback system changes. Attempt
 * remaining safe cleanup after errors; never claim failed cleanup succeeded.
 * No steady-state allocation, unbounded retry/queue or per-frame logging. Only
 * acquire/encode calls are designated media wait points, bounded by deadline;
 * input calls never wait on media, network, statistics or browser work. */
typedef enum { SP5_OK = 0, SP5_ERROR } sp5_status;

typedef struct {
    sp5_status status;
    sp5_reason reason;
} sp5_result;

typedef struct {
    const uint8_t *data;
    size_t size;
} sp5_bytes;

/* Caller owns writable storage. Callee never changes data/capacity, never writes
 * past capacity; size is valid only on success and never exceeds capacity. */
typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t size;
} sp5_buffer;

typedef enum {
    SP5_PROTECTION_UNKNOWN = 0,
    SP5_PROTECTION_CLEAR,
    SP5_PROTECTION_ACTIVE
} sp5_protection;

typedef enum { SP5_DYNAMIC_RANGE_UNKNOWN = 0, SP5_SDR, SP5_HDR_PQ_BT2020 } sp5_dynamic_range;

/* Metadata is an opaque bounded description in a sourced, named format, NOT a
 * presumed console struct. Reference/provenance strings remain valid until close.
 * Missing source metadata stays unavailable; helper defaults/pixels are no oracle. */
typedef struct {
    sp5_validity validity;
    sp5_reason reason;
    const char *format_ref;
    const char *source_evidence_ref;
    sp5_bytes payload;
} sp5_hdr_metadata;

/* Increment epoch on output format, HDR metadata, or protection changes. Every
 * acquired video/audio lease carries this epoch. Invalidate queued old content
 * at protection transitions, including pre-protection audio; unknown is not clear.
 * Core emits placeholder/stereo silence while protected/unknown, never forwards
 * source bytes, and resumes only after clear state is established. */
typedef struct {
    uint64_t epoch;
    sp5_timestamp observed_at;
    sp5_protection protection;
    sp5_dynamic_range dynamic_range;
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    sp5_hdr_metadata hdr;
} sp5_source_state;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps_numerator;
    uint32_t fps_denominator;
    const char *codec;
    sp5_dynamic_range dynamic_range;
    uint32_t chosen_bitrate_kbps;
} sp5_media_config;

/* Source provenance and validated modes/bitrate limits are supplied by adapter
 * evidence, never inferred from a non-null callback or successful SDK link. */
typedef struct {
    bool synthetic;
    const char *implementation_ref;
    const char *firmware;
    const char *capability_evidence_ref;
    sp5_validity validity;
    sp5_reason reason;
} sp5_platform_provenance;

/* Opaque surfaces may be hardware-resident. No portable code dereferences them.
 * An acquire success transfers exactly one read-only lease to caller; release
 * returns it exactly once even on encode failure. IDs are unique while live.
 * Source pixel layout is described by a sourced format_ref, never guessed. */
typedef struct {
    uint64_t lease_id;
    uint64_t frame_id;
    uint64_t source_epoch;
    const void *surface;
    const char *format_ref;
    sp5_timestamp capture_timestamp;
    sp5_stage_span capture_span;
} sp5_video_frame;

typedef struct {
    void *context;
    sp5_result (*open)(void *context, const sp5_media_config *config);
    sp5_result (*source_state)(void *context, sp5_source_state *state);
    /* A non-clear source returns no source frame; reason explains protection.
     * deadline is a valid timestamp in adapter clock; metrics=false forbids
     * extra detailed timestamps but retains the FR-021 capture timestamp. */
    sp5_result (*acquire)(void *context, sp5_timestamp deadline, bool metrics,
                          sp5_video_frame *frame);
    sp5_result (*release)(void *context, uint64_t lease_id);
    sp5_result (*close)(void *context);
} sp5_capture_ops;

typedef struct {
    uint64_t frame_id;
    uint64_t source_epoch;
    bool key_frame;
    sp5_stage_span encode_span;
} sp5_encoded_frame;

typedef struct {
    void *context;
    sp5_result (*open)(void *context, const sp5_media_config *config);
    /* Borrows frame only until return, including on failure; no retained lease.
     * Production success means completed HARDWARE bitstream in caller storage;
     * synthetic fakes report their provenance and cannot prove hardware use. Configured
     * HDR/SDR conversion must be validated; no CPU software-encoding fallback.
     * metrics=false leaves encode_span unavailable, without extra timestamps. */
    sp5_result (*encode)(void *context, const sp5_video_frame *frame, bool force_key_frame,
                         sp5_timestamp deadline, bool metrics, sp5_buffer *output,
                         sp5_encoded_frame *encoded);
    sp5_result (*close)(void *context);
} sp5_encode_ops;

typedef struct {
    uint64_t lease_id;
    uint64_t source_epoch;
    sp5_timestamp timestamp;
    sp5_bytes samples;
    uint32_t sample_rate;
    uint32_t frames;
    uint8_t channels;
    const char *sample_format_ref;
} sp5_audio_block;

typedef struct {
    void *context;
    sp5_result (*open)(void *context, uint32_t sample_rate);
    /* Same lease rules as video; channels MUST be two. Protected/unknown source
     * returns no source samples; intentional protected silence is not a missing
     * ordinary game-audio observation. Payload valid only until release. */
    sp5_result (*acquire)(void *context, sp5_timestamp deadline, sp5_audio_block *block);
    sp5_result (*release)(void *context, uint64_t lease_id);
    sp5_result (*close)(void *context);
} sp5_audio_ops;

enum { SP5_MAX_CONTROLLERS = 4, SP5_MAX_TOUCHES = 2 };

typedef struct {
    bool active;
    uint32_t contact_id;
    float x;
    float y;
    /* mcc 41cedb8 Input.h:166-177: normalized [0,1] pressure and
     * touchpadIndex (0/1). These describe client data, not a console ABI. */
    float pressure;
    uint8_t touchpad_index;
} sp5_touch;

/* Portable normalized controller state, not PS5 or Moonlight wire layout.
 * Axis/trigger ranges and button mapping are sourced by input translation;
 * touch x/y are finite [0,1], gyro rad/s and acceleration m/s^2 are finite.
 * Optional capability flags gate motion/touch; no keyboard/mouse interface. */
typedef struct {
    uint32_t buttons;
    int16_t axes[4];
    uint16_t triggers[2];
    bool has_motion;
    float gyro[3];
    float acceleration[3];
    bool has_touch;
    sp5_touch touches[SP5_MAX_TOUCHES];
} sp5_controller_state;

typedef struct {
    uint32_t controller_id;
    uint16_t low_frequency;
    uint16_t high_frequency;
} sp5_controller_feedback;

typedef struct {
    void *context;
    /* Logical IDs are assigned by core in connection order, at most four live.
     * Failed add leaves no controller; remove/release_all are idempotent and
     * handle partially opened controllers without touching other sessions. */
    sp5_result (*add)(void *context, uint64_t session_id, uint32_t controller_id);
    /* Caller supplies authenticated event IDs and pre-parse host receipt.
     * Success alone does NOT assert delivery. Set CONFIRMED only at a sourced
     * console-consumption boundary; otherwise UNCONFIRMED + absent timestamp.
     * metrics=false forbids extra delivery timing; retain functional result.
     * No pointer is retained after return. */
    sp5_result (*deliver)(void *context, const sp5_controller_state *state, bool metrics,
                          sp5_input_evidence *evidence);
    sp5_result (*poll_feedback)(void *context, sp5_controller_feedback *feedback);
    sp5_result (*remove)(void *context, uint64_t session_id, uint32_t controller_id);
    /* First teardown operation: clear buttons/sticks/triggers/transient state,
     * neutralize where required, remove all session controllers. Return safety
     * errors explicitly; never wait for drains/exports or retry indefinitely. */
    sp5_result (*release_all)(void *context, uint64_t session_id);
} sp5_input_ops;

typedef struct {
    bool remote_play;
    bool share_play;
    bool stopping_or_rest;
    sp5_validity validity;
    sp5_reason reason;
} sp5_system_state;

typedef struct {
    void *context;
    /* Probe does not install hooks. Unsupported firmware is refused; never guess
     * offsets. Each reversible change needs per-firmware source/rollback records.
     * Failed prepare unwinds its partial changes. Rollback is idempotent and
     * applies only owned changes, preserves foreground game and homebrew setup. */
    sp5_result (*probe)(void *context, sp5_platform_provenance *provenance);
    sp5_result (*prepare)(void *context);
    sp5_result (*state)(void *context, sp5_system_state *state);
    sp5_result (*notify)(void *context, const char *message, size_t size);
    sp5_result (*rollback)(void *context);
} sp5_system_ops;

typedef struct {
    void *context;
    sp5_result (*describe)(void *context, sp5_clock_description *description);
    /* No wall clock for elapsed measurements. Fault/discontinuity invalidates
     * affected measurements; unknown resolution/uncertainty is not accuracy.
     * Reference-machine clocks require independently established mapping. */
    sp5_result (*now)(void *context, sp5_timestamp *timestamp);
} sp5_clock_ops;

#endif
