#ifndef SUNSHINE_PLATFORM_AUDIO_PS5_H
#define SUNSHINE_PLATFORM_AUDIO_PS5_H

#include "platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PROVISIONAL production adapter for firmware 13.60, not device-qualified.
 *
 * Source of the Avcap2 ABI: tools/ps5-probes/media_probe.h and
 * tools/ps5-probes/capture_encode.c (T041), retaining the five audio-call NIDs,
 * the 56-byte OpenAudio request, and the 8192-byte NOWAIT ReadAudio record from
 * SoloBridge 71fc2af65e04b188ed364274f58351f8f413fc60 solobridge/capture.c and
 * HearBridge 5da0744d28fe360d6cb236999800e8a4de042167 src/avcap2.c. The only
 * admitted request is size=0x38, unknown08=0x18, source=2 (Remote Play mix),
 * mode=0 (raw stereo float), unknown1c=UINT32_MAX, all other fields zero.
 * Record layout is native little-endian float32 stereo at 48 kHz; the probe
 * observed 239,616 frames in 5.0009 s with clean Stop/Close/Terminate/module
 * unload (T041 2026-10-25 run). See
 * specs/001-moonlight-streaming-host/research-r3-findings.md and
 * tools/ps5-probes/README.md.
 *
 * Ipmi/ResourceArbitrator/Avcap2 loading plus Initialize/Terminate and module
 * unload belong to platform/ps5_media_global.c. open() takes one global
 * reference and resolves only OpenAudio/Start/Stop/Close/ReadAudio from the
 * global Avcap2 handle; close() returns its reference after local handle
 * teardown.
 *
 * Deliberately unresolved and therefore not claimed by this adapter:
 * - Avcap2 exposes no readable content-protection state. The caller must feed a
 *   sourced observation through sp5_ps5_audio_observe_source() after open(); the
 *   default (and every fresh open) is UNKNOWN and acquire() returns no source
 *   samples. ACTIVE/UNKNOWN never emit source bytes.
 * - ReadAudio's 16-byte metadata is opaque. Its first u32 is initialized to
 *   0x10 by HearBridge, but input/output direction, timestamp units and source
 *   epoch are not established. sp5_audio_block.timestamp is therefore explicitly
 *   UNAVAILABLE/STAGE_UNAVAILABLE; no source timestamp is invented.
 * - Stereo channel order, source clock, driver/service accounting, service-call
 *   deadlines and protected-content behavior remain unverified.
 * - A non-null handle returned with a failed OpenAudio is not owned and is never
 *   passed to Close, matching the probe's documented uncertainty.
 * - Only one audio context is supported per process (a static owner guard) and
 *   the caller serializes access; no locks are hidden here.
 *
 * Storage is caller-owned and fixed. open() acquires the shared global reference
 * and handle; acquire() never allocates. close() is idempotent: it retains the
 * first cleanup diagnostic, keeps accurate handle/global ownership, allows a
 * later retry after Stop/Close failure, and returns success once every resource
 * this context owns is actually released.
 */
enum {
    SP5_PS5_AUDIO_SAMPLE_RATE = 48000,
    SP5_PS5_AUDIO_CHANNELS = 2,
    SP5_PS5_AUDIO_FRAMES = 1024,
    SP5_PS5_AUDIO_SAMPLES = SP5_PS5_AUDIO_FRAMES * SP5_PS5_AUDIO_CHANNELS,
    SP5_PS5_AUDIO_BYTES = SP5_PS5_AUDIO_SAMPLES * 4,
    SP5_PS5_AUDIO_METADATA_BYTES = 16,
    SP5_PS5_AUDIO_MAX_READS_PER_ACQUIRE = 5000
};

/* Console CLOCK_MONOTONIC, truncated to microseconds as in the probe. This is
 * an adapter observation clock, not an Avcap2 source timestamp. */
#define SP5_PS5_AUDIO_CLOCK_DOMAIN UINT64_C(0x505335415544494f)
#define SP5_PS5_AUDIO_CLOCK_EPOCH UINT64_C(1)
#define SP5_PS5_AUDIO_CLOCK_TICKS_PER_SECOND UINT64_C(1000000)
#define SP5_PS5_AUDIO_SAMPLE_FORMAT_REF                                                            \
    "avcap2/raw-f32le-stereo-48000 (SoloBridge 71fc2af/HearBridge 5da0744; "                       \
    "channel order unverified)"

/* Exact public caller signatures from the probe. These are stored in the
 * preallocated context; no SDK types are exposed. Initialize/Terminate are not
 * here because the shared media owner owns them. */
typedef struct {
    int (*open)(void **, const void *, uint8_t);
    int (*start)(void *);
    int (*stop)(void *);
    int (*close)(void *);
    int (*read)(void *, void *, size_t, void *, uint32_t);
} sp5_ps5_audio_api;

typedef struct {
    sp5_ps5_audio_api api;
    void *handle;
    bool global_acquired;
    bool global_release_pending;
    bool started;
    bool open;
    bool leased;
    bool source_observed;
    bool cleanup_reported;
    sp5_protection protection;
    sp5_reason cleanup_error;
    uint64_t source_epoch;
    uint64_t next_lease;
    uint64_t lease_id;
    uint64_t reads;
    uint64_t empties;
    uint64_t overruns;
    uint32_t sample_rate;
    _Alignas(16) float samples[SP5_PS5_AUDIO_SAMPLES];
    _Alignas(16) uint8_t metadata[SP5_PS5_AUDIO_METADATA_BYTES];
} sp5_ps5_audio;

void sp5_ps5_audio_init(sp5_ps5_audio *audio);
sp5_audio_ops sp5_ps5_audio_ops(sp5_ps5_audio *audio);

/* Caller-supplied sourced protection/epoch observation. The observation is
 * accepted only with a valid observed_at timestamp; protection remains the
 * caller's evidence, not an Avcap2 getter. */
sp5_result sp5_ps5_audio_observe_source(sp5_ps5_audio *audio, const sp5_source_state *state);

/* Adapter clock used to validate acquire deadlines. */
sp5_result sp5_ps5_audio_now(sp5_timestamp *timestamp);

#endif
