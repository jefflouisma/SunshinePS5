#ifndef SUNSHINE_TESTS_FAKES_PLATFORM_H
#define SUNSHINE_TESTS_FAKES_PLATFORM_H

#include "runtime.h"

/* Synthetic test fixtures only: no device, firmware, codec or timing evidence.
 * Single-threaded deterministic driver; independent contexts can run in parallel.
 * All state is caller-owned, with one video/audio lease and four controllers. */
typedef enum {
    SP5_FAKE_CAPTURE_OPEN = 0,
    SP5_FAKE_SOURCE_STATE,
    SP5_FAKE_CAPTURE_ACQUIRE,
    SP5_FAKE_CAPTURE_RELEASE,
    SP5_FAKE_CAPTURE_CLOSE,
    SP5_FAKE_ENCODER_OPEN,
    SP5_FAKE_ENCODE,
    SP5_FAKE_ENCODER_CLOSE,
    SP5_FAKE_AUDIO_OPEN,
    SP5_FAKE_AUDIO_ACQUIRE,
    SP5_FAKE_AUDIO_RELEASE,
    SP5_FAKE_AUDIO_CLOSE,
    SP5_FAKE_INPUT_ADD,
    SP5_FAKE_INPUT_DELIVER,
    SP5_FAKE_INPUT_FEEDBACK,
    SP5_FAKE_INPUT_REMOVE,
    SP5_FAKE_INPUT_RELEASE_ALL,
    SP5_FAKE_SYSTEM_PROBE,
    SP5_FAKE_SYSTEM_PREPARE,
    SP5_FAKE_SYSTEM_STATE,
    SP5_FAKE_SYSTEM_NOTIFY,
    SP5_FAKE_SYSTEM_ROLLBACK,
    SP5_FAKE_CLOCK_DESCRIBE,
    SP5_FAKE_CLOCK_NOW,
    SP5_FAKE_OPERATION_COUNT
} sp5_fake_operation;

typedef struct {
    bool live;
    uint64_t session_id;
    sp5_controller_state state;
} sp5_fake_controller;

typedef struct {
    sp5_capture_ops capture;
    sp5_encode_ops encoder;
    sp5_audio_ops audio;
    sp5_input_ops input;
    sp5_system_ops system;
    sp5_clock_ops clock;
    /* Set one reason to fail the next call of that operation, then auto-clear.
     * Failures never acquire resources; failed cleanup leaves ownership visible. */
    sp5_reason fail_next[SP5_FAKE_OPERATION_COUNT];
    uint64_t calls[SP5_FAKE_OPERATION_COUNT];
    uint64_t sequence;
    uint64_t last_input_release_sequence;
    uint64_t last_media_close_sequence;
    uint64_t capture_closes;
    uint64_t encoder_closes;
    uint64_t audio_closes;
    uint64_t rollbacks;
    uint64_t next_lease;
    uint64_t video_lease;
    uint64_t audio_lease;
    uint64_t frame_id;
    bool capture_open;
    bool encoder_open;
    bool audio_open;
    bool video_leased;
    bool audio_leased;
    bool prepared;
    bool confirm_delivery;
    uint32_t sample_rate;
    sp5_source_state source;
    sp5_system_state system_state;
    sp5_timestamp now;
    sp5_clock_description clock_description;
    sp5_fake_controller controllers[SP5_MAX_CONTROLLERS];
    uint16_t video_samples[8];
    int16_t audio_samples[8];
    uint8_t hdr_metadata[4];
} sp5_fake_platform;

/* Initialize before any ops; do not copy/reinitialize a live context, since
 * operation contexts and borrowed sample/metadata spans refer to this object. */
void sp5_fake_init(sp5_fake_platform *fake);
sp5_result sp5_fake_advance(sp5_fake_platform *fake, uint64_t ticks);
sp5_result sp5_fake_set_source(sp5_fake_platform *fake, sp5_protection protection,
                               sp5_dynamic_range range);

#endif
