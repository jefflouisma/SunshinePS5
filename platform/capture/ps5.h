#ifndef SUNSHINE_PLATFORM_CAPTURE_PS5_H
#define SUNSHINE_PLATFORM_CAPTURE_PS5_H

#include "platform.h"

#include <stdint.h>
#include <string.h>

/* Provisional production capture adapter for the sourced firmware-13.60
 * Avcap2 path. Source of every layout/NID below:
 *   tools/ps5-probes/capture_encode.c (video_request_init, video_start,
 *   video_read, video_cleanup, video_probe) and media_probe.h.
 * No direct capture surface allocation exists in the sourced mode-0 request:
 * Sony's Remote Play caller supplies a 0x2e8-byte request and Avcap2 returns
 * encoded Annex-B records through ReadVideo. This adapter therefore owns one
 * fixed 2 MiB read buffer and one 112-byte opaque info record.
 *
 * Protection is always UNKNOWN here: no observable protection signal exists in
 * R3. acquire() therefore returns SP5_REASON_PROTECTION_UNKNOWN and no frame
 * unless a later sourced boundary calls sp5_ps5_capture_observe_protection().
 * That setter is not called by this adapter; CLEAR can never be self-asserted.
 *
 * Avcap2 Initialize/Terminate and module load/unload are owned by
 * ps5_media_global.c. Capture owns only its ResourceArbitrator selector
 * request/stop and its video session. HDR_PQ_BT2020 is refused until the
 * request/format mapping supports HDR. */

enum {
    SP5_PS5_CAPTURE_REQUEST_BYTES = 0x2e8,
    SP5_PS5_CAPTURE_INFO_BYTES = 112,
    SP5_PS5_CAPTURE_STATE_BYTES = 0x20,
    SP5_PS5_CAPTURE_READ_BYTES = 0x200000,
    SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES = 0x1a8,
    SP5_PS5_CAPTURE_PARAMETER_SLOT_BYTES = 128,
    SP5_PS5_CAPTURE_PARAMETER_BYTES = 3 * (4 + SP5_PS5_CAPTURE_PARAMETER_SLOT_BYTES),
    SP5_PS5_CAPTURE_SELECTOR_COUNT = 32,
    SP5_PS5_CAPTURE_QUERY_ATTEMPTS = 64,
    SP5_PS5_CAPTURE_RESOURCE_QUERIES = 320,
    SP5_PS5_CAPTURE_CLEANUP_QUERIES = 64,
    SP5_PS5_CAPTURE_DELAY_US = 16000,
    SP5_PS5_CAPTURE_RESOURCE_ID = 0xb4,
    SP5_PS5_CAPTURE_SURFACE_MAGIC = 0x53503543, /* "SP5C" */
    SP5_PS5_CAPTURE_SURFACE_VERSION = 1
};

/* Exact transcription of the retained probe's Avcap2 configuration-header
 * parser: tools/ps5-probes/capture_encode.c:581-620
 * video_collect_header_packet(). Avcap2 query_headers ("VAFgNc0Pfp8") returns
 * a 0x1a8-byte packet whose +0x00 u64 is its own size and +0x08 u32 is the
 * codec selector. AVC SPS/PPS (NAL 7/8) occupy slots +0x10/+0x98; HEVC uses
 * VPS/SPS/PPS (NAL 32/33/34) at +0x120/+0x10/+0x98, exactly as the probe's
 * (i + 2) % 3 mapping selects. Slot sizes are at +0x10/+0x98/+0x120 and
 * payloads at +0x18/+0xa0/+0x128, each 1..128 bytes (HEVC >=2). Writes
 * 00 00 00 01 + payload records in SPS/PPS or VPS/SPS/PPS order and returns
 * their byte count, or 0 when the packet is not exactly the sourced shape. No
 * partial record is written. This is configuration-header signaling, not an
 * observed VCL decode. Exposed here so the portable unit test exercises the
 * production parser. */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static inline size_t sp5_ps5_parameter_sets_parse(uint8_t *sets, size_t capacity,
                                                  const uint8_t *packet, size_t size,
                                                  uint32_t codec)
{
    if (sets == NULL || packet == NULL || capacity == 0 || size != 0x1a8 || codec > 1)
        return 0;
    uint64_t packet_size = 0;
    uint32_t packet_codec = 0;
    memcpy(&packet_size, packet, sizeof(packet_size));
    memcpy(&packet_codec, packet + 8, sizeof(packet_codec));
    if (packet_size != size || packet_codec != codec)
        return 0;
    static const size_t lengths[] = {0x10, 0x98, 0x120};
    static const size_t payloads[] = {0x18, 0xa0, 0x128};
    static const uint32_t avc_types[] = {7, 8};
    static const uint32_t hevc_types[] = {32, 33, 34};
    size_t count = codec == 0 ? 2 : 3;
    uint64_t sizes[3] = {0};
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t slot = codec == 0 ? i : (i + 2) % 3;
        memcpy(&sizes[i], packet + lengths[slot], sizeof(sizes[i]));
        if (sizes[i] < (codec == 0 ? 1U : 2U) || sizes[i] > SP5_PS5_CAPTURE_PARAMETER_SLOT_BYTES)
            return 0;
        uint32_t type =
            codec == 0 ? packet[payloads[slot]] & 31U : (packet[payloads[slot]] >> 1) & 63U;
        if (type != (codec == 0 ? avc_types[i] : hevc_types[i]))
            return 0;
        total += 4 + (size_t)sizes[i];
    }
    if (total > capacity)
        return 0;
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t slot = codec == 0 ? i : (i + 2) % 3;
        sets[used++] = 0;
        sets[used++] = 0;
        sets[used++] = 0;
        sets[used++] = 1;
        memcpy(sets + used, packet + payloads[slot], (size_t)sizes[i]);
        used += (size_t)sizes[i];
    }
    return used;
}

/* Internal, provisional producer/consumer descriptor. frame->surface points at
 * one of these in the capture context; the bytes it references live in the same
 * context and are valid only between acquire() success and release(). Portable
 * core code never dereferences it. The encode adapter validates magic/version
 * plus the exact format_ref string before borrowing. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    const uint8_t *data;
    size_t size;
    uint32_t codec; /* SP5_PS5_CODEC_* */
    uint32_t key_frame;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t reserved;
    uint64_t frame_id;
} sp5_ps5_capture_surface;

typedef int (*sp5_ps5_capture_open_fn)(void **, const void *, uint8_t);
typedef int (*sp5_ps5_capture_start_fn)(void *);
typedef int (*sp5_ps5_capture_stop_fn)(void *);
typedef int (*sp5_ps5_capture_close_fn)(void *);
typedef int (*sp5_ps5_capture_read_fn)(void *, void *, size_t, void *, uint32_t);
typedef int (*sp5_ps5_capture_query_flags_fn)(uint32_t *);
typedef int (*sp5_ps5_capture_query_state_fn)(void *);
/* Avcap2 "VAFgNc0Pfp8": request+8 in, 0x1a8-byte configuration packet out. */
typedef int (*sp5_ps5_capture_query_headers_fn)(const void *, void *);
typedef void (*sp5_ps5_resource_callback)(int32_t, int32_t, const void *);
typedef int (*sp5_ps5_resource_register_fn)(int32_t, sp5_ps5_resource_callback, void *);
typedef int (*sp5_ps5_resource_unregister_fn)(int32_t);
typedef int (*sp5_ps5_resource_request_fn)(int32_t, const char *, void *, int32_t);
typedef int (*sp5_ps5_resource_query_fn)(int32_t *);

typedef struct {
    sp5_ps5_capture_open_fn open;
    sp5_ps5_capture_start_fn start;
    sp5_ps5_capture_stop_fn stop;
    sp5_ps5_capture_close_fn close;
    sp5_ps5_capture_read_fn read;
    sp5_ps5_capture_query_flags_fn query_mode_flags;
    sp5_ps5_capture_query_state_fn query_capture_state;
    sp5_ps5_capture_query_headers_fn query_headers;
} sp5_ps5_capture_api;

typedef struct {
    sp5_ps5_resource_register_fn register_client;
    sp5_ps5_resource_unregister_fn unregister_client;
    sp5_ps5_resource_request_fn request_state;
    sp5_ps5_resource_query_fn query_selectors;
} sp5_ps5_resource_api;

typedef struct {
    sp5_ps5_capture_api api;
    sp5_ps5_resource_api resource;
    void *handle;
    bool global_acquired;
    bool started;
    bool opened;
    bool ever_registered;
    bool registered;
    bool requested;
    bool unregistered;
    bool selector_removed;
    bool closed;
    bool protection_observed;
    bool cleanup_gate_failed;
    bool failed_open_unproven;
    uint32_t mode_flags;
    uint32_t tier;
    uint32_t fps;
    uint32_t codec;
    uint32_t depth;
    uint32_t width;
    uint32_t height;
    uint32_t chosen_bitrate_kbps;
    sp5_dynamic_range dynamic_range;
    sp5_protection protection;
    const char *protection_evidence_ref;
    uint64_t source_epoch;
    uint64_t next_lease;
    uint64_t live_lease;
    uint64_t next_frame_id;
    bool leased;
    sp5_reason cleanup_failure;
    size_t parameter_size;
    sp5_ps5_capture_surface surface;
    _Alignas(16) uint8_t request[SP5_PS5_CAPTURE_REQUEST_BYTES];
    _Alignas(16) uint8_t info[SP5_PS5_CAPTURE_INFO_BYTES];
    _Alignas(16) uint8_t capture_state[SP5_PS5_CAPTURE_STATE_BYTES];
    _Alignas(16) uint8_t parameters[SP5_PS5_CAPTURE_PARAMETER_BYTES];
    _Alignas(16) uint8_t read_buffer[SP5_PS5_CAPTURE_READ_BYTES];
    int32_t selectors[SP5_PS5_CAPTURE_SELECTOR_COUNT];
} sp5_ps5_capture;

/* Caller owns the context (static/session pool). Zeroes it; no allocation. */
void sp5_ps5_capture_init(sp5_ps5_capture *capture);
sp5_capture_ops sp5_ps5_capture_operations(sp5_ps5_capture *capture);
void sp5_ps5_capture_provenance(const sp5_ps5_capture *capture,
                                sp5_platform_provenance *provenance);

/* Borrows the parsed Annex-B parameter sets from the last successful Avcap2
 * header query until the capture is closed. Returns false and clears outputs
 * when no valid SPS/PPS (AVC) or VPS/SPS/PPS (HEVC) packet was parsed. */
bool sp5_ps5_capture_parameters(const sp5_ps5_capture *capture, const uint8_t **data, size_t *size);

/* Future sourced protection boundary only. No R3 getter exists, so production
 * code does not call this. A CLEAR observation requires a non-empty evidence
 * reference; UNKNOWN/ACTIVE do not. Changes increment source_epoch. */
sp5_result sp5_ps5_capture_observe_protection(sp5_ps5_capture *capture, sp5_protection protection,
                                              const char *evidence_ref);

#endif
