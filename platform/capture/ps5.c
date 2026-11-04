#include "capture/ps5.h"

#include "firmware/13.60.h"
#include "ps5_annexb.h"
#include "ps5_clock.h"
#include "ps5_media.h"
#include "ps5_media_global.h"
#include "runtime.h"

#include <limits.h>
#include <string.h>
#include <unistd.h>

#if defined(__PROSPERO__)
#include <ps5/kernel.h>
#endif

// NOLINTBEGIN(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)

/* Exact public-caller NIDs used by the retained T041 probe. Module lifetime
 * and Avcap2 Initialize/Terminate are owned by ps5_media_global.c. */
#define SP5_AVCAP2_OPEN "4jwUgirLFrA"
#define SP5_AVCAP2_START "8CGNCwBsItI"
#define SP5_AVCAP2_STOP "Z5dKK0xnQ8g"
#define SP5_AVCAP2_CLOSE "tR7gPe1i8hw"
#define SP5_AVCAP2_READ "pN0hIQZsMzc"
#define SP5_AVCAP2_MODE_FLAGS "DHFDHdKAr9U"
#define SP5_AVCAP2_CAPTURE_STATE "RW88AiwmL+Y"
#define SP5_AVCAP2_QUERY_HEADERS "VAFgNc0Pfp8"
#define SP5_RESOURCE_REGISTER "kJvjeet+f9I"
#define SP5_RESOURCE_UNREGISTER "LqmYZGB-UEQ"
#define SP5_RESOURCE_REQUEST "CYjfezkWcaE"
#define SP5_RESOURCE_QUERY "fBX+DBOwNis"

#define SP5_AVCAP2_STATUS_RETRY UINT32_C(0x81950008)
#define SP5_AVCAP2_EMPTY_1 UINT32_C(0x81950002)
#define SP5_AVCAP2_EMPTY_2 UINT32_C(0x8195000d)
#define SP5_AVCAP2_OVERRUN UINT32_C(0x81950004)

static void store32(uint8_t *bytes, size_t offset, uint32_t value)
{
    memcpy(bytes + offset, &value, sizeof(value));
}

static void store64(uint8_t *bytes, size_t offset, uint64_t value)
{
    memcpy(bytes + offset, &value, sizeof(value));
}

static uint32_t load32(const uint8_t *bytes, size_t offset)
{
    uint32_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static bool bind_pointer(void *destination, size_t capacity, uintptr_t address)
{
    /* The SDK resolver returns a flat 64-bit address. Refuse null or an
     * incompatible pointer representation instead of guessing a function type. */
    _Static_assert(sizeof(uintptr_t) == 8, "sourced SDK resolver is 64-bit");
    return address != 0 && capacity == sizeof(address) &&
           memcpy(destination, &address, sizeof(address)) == destination;
}

static uintptr_t dynlib_resolve(pid_t pid, uint32_t handle, const char *nid)
{
#if defined(__PROSPERO__)
    return (uintptr_t)kernel_dynlib_resolve(pid, handle, nid);
#else
    (void)pid;
    (void)handle;
    (void)nid;
    return 0;
#endif
}

static bool delay_us(uint32_t microseconds)
{
    struct timespec delay = {.tv_sec = 0, .tv_nsec = (long)microseconds * 1000L};
    return nanosleep(&delay, NULL) == 0;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void resource_notified(int32_t resource_id, int32_t operation, const void *opaque)
{
    (void)resource_id;
    (void)operation;
    (void)opaque;
    /* Registration needs a callback; selector state is read by query_selectors. */
}

static uint64_t bitrate_bits_per_second(uint32_t kbps)
{
    return (uint64_t)kbps * 1000;
}

static bool request_build(sp5_ps5_capture *capture)
{
    if (capture->chosen_bitrate_kbps == 0 ||
        capture->chosen_bitrate_kbps > (uint32_t)(INT32_MAX / 1000))
        return false;
    uint8_t *request = capture->request;
    memset(request, 0, sizeof(capture->request));
    uint64_t bitrate = bitrate_bits_per_second(capture->chosen_bitrate_kbps);
    /* SceRemotePlay 0x41c081e2-0x41c08226 writes request +0xcc as
     * `bitrate_kbps * 1000`, +0xc8 as that value divided by 1.12/1.10, and
     * +0xd0 as bitrate*1001/(fps*1000). The earlier probe's kbps*125
     * transcription was bytes/s and under-requested by 8x; the measured
     * 2.85 Mbps from a 20,000 kbps request confirms that unit error. */
    store64(request, 0x00, SP5_PS5_CAPTURE_REQUEST_BYTES);
    store64(request, 0x08, 0x28);
    store32(request, 0x14, 2);
    store32(request, 0x28, SP5_PS5_CAPTURE_RESOURCE_ID);
    store32(request, 0x18, capture->tier);
    store32(request, 0x1c, capture->fps != 30);
    store32(request, 0x98, 1);
    store32(request, 0xc8, (uint32_t)((float)bitrate / 1.12f));
    store32(request, 0xcc, (uint32_t)bitrate);
    store32(request, 0xd0, (uint32_t)(bitrate * 1001 / ((uint64_t)capture->fps * 1000)));
    store32(request, 0x190, 1);
    store32(request, 0x194, 1);
    store64(request, 0x1e0, 0x88);
    store64(request, 0x1e8, UINT64_C(0x3ff0000000000000));
    store32(request, 0x20c, 384);
    store32(request, 0x210, 384);
    store32(request, 0x214, 512);
    store64(request, 0x218, 0x48000);
    /* +0x20 depth preset and +0x24 codec; Venc rejects ten-bit AVC. */
    if (capture->codec > SP5_PS5_CODEC_HEVC ||
        (capture->depth != SP5_PS5_DEPTH_8 && capture->depth != SP5_PS5_DEPTH_10) ||
        (capture->codec == SP5_PS5_CODEC_AVC && capture->depth != SP5_PS5_DEPTH_8))
        return false;
    store32(request, 0x20, capture->depth == SP5_PS5_DEPTH_10);
    store32(request, 0x24, capture->codec);
    uint32_t peak = load32(request, 0xcc);
    store32(request, 0xc8,
            (uint32_t)((float)peak / (capture->codec == SP5_PS5_CODEC_AVC ? 1.12f : 1.10f)));
    return true;
}

static void request_apply_state(sp5_ps5_capture *capture)
{
    uint32_t selected_mode = 2;
    if ((capture->mode_flags & 1U) != 0)
        selected_mode = 6;
    else if ((capture->mode_flags & 2U) != 0)
        selected_mode = 8;
    store32(capture->request, 0x2a8, capture->mode_flags);
    store32(capture->request, 0x2ac, selected_mode);
    capture->request[0x2bc] = (uint8_t)(capture->capture_state[0x17] ^ 1U);
}

static bool resolve_api(sp5_ps5_capture *capture, uint32_t module)
{
    pid_t pid = getpid();
#define SP5_RESOLVE(field, target, nid)                                                            \
    do {                                                                                           \
        uintptr_t address = dynlib_resolve(pid, module, nid);                                      \
        if (!bind_pointer((void *)&(target).field, sizeof((target).field), address))               \
            return false;                                                                          \
    } while (0)
    SP5_RESOLVE(open, capture->api, SP5_AVCAP2_OPEN);
    SP5_RESOLVE(start, capture->api, SP5_AVCAP2_START);
    SP5_RESOLVE(stop, capture->api, SP5_AVCAP2_STOP);
    SP5_RESOLVE(close, capture->api, SP5_AVCAP2_CLOSE);
    SP5_RESOLVE(read, capture->api, SP5_AVCAP2_READ);
    SP5_RESOLVE(query_mode_flags, capture->api, SP5_AVCAP2_MODE_FLAGS);
    SP5_RESOLVE(query_capture_state, capture->api, SP5_AVCAP2_CAPTURE_STATE);
    SP5_RESOLVE(query_headers, capture->api, SP5_AVCAP2_QUERY_HEADERS);
#undef SP5_RESOLVE
    return true;
}

static bool resolve_resource(sp5_ps5_capture *capture, uint32_t module)
{
    pid_t pid = getpid();
#define SP5_RESOLVE(field, nid)                                                                    \
    do {                                                                                           \
        uintptr_t address = dynlib_resolve(pid, module, nid);                                      \
        if (!bind_pointer((void *)&capture->resource.field, sizeof(capture->resource.field),       \
                          address))                                                                \
            return false;                                                                          \
    } while (0)
    SP5_RESOLVE(register_client, SP5_RESOURCE_REGISTER);
    SP5_RESOLVE(unregister_client, SP5_RESOURCE_UNREGISTER);
    SP5_RESOLVE(request_state, SP5_RESOURCE_REQUEST);
    SP5_RESOLVE(query_selectors, SP5_RESOURCE_QUERY);
#undef SP5_RESOLVE
    return true;
}

static bool resource_query(sp5_ps5_capture *capture, bool *available)
{
    for (size_t i = 0; i < SP5_PS5_CAPTURE_SELECTOR_COUNT; ++i)
        capture->selectors[i] = INT32_MIN;
    *available = false;
    if (capture->resource.query_selectors(capture->selectors) != 0)
        return false;
    for (size_t i = 0; i < SP5_PS5_CAPTURE_SELECTOR_COUNT; ++i)
        *available = *available || capture->selectors[i] == SP5_PS5_CAPTURE_RESOURCE_ID;
    return true;
}

static sp5_result resource_start(sp5_ps5_capture *capture)
{
    bool available = false;
    if (!resource_query(capture, &available))
        return sp5_failure(SP5_REASON_PLATFORM);
    if (available)
        return sp5_failure(SP5_REASON_OWNERSHIP); /* selector already held by another client */
    int rc =
        capture->resource.register_client(SP5_PS5_CAPTURE_RESOURCE_ID, resource_notified, capture);
    capture->registered = rc >= 0;
    capture->ever_registered = capture->registered;
    if (rc != 0)
        return sp5_failure(SP5_REASON_PLATFORM);
    rc = capture->resource.request_state(SP5_PS5_CAPTURE_RESOURCE_ID, "RUNNING", capture,
                                         (int32_t)getpid());
    /* The probe marks RUNNING requested before the call, so a failed request
     * still gets a STOP during cleanup. */
    capture->requested = true;
    if (rc != 0)
        return sp5_failure(SP5_REASON_PLATFORM);
    for (unsigned attempt = 0; attempt < SP5_PS5_CAPTURE_RESOURCE_QUERIES; ++attempt) {
        if (!resource_query(capture, &available))
            return sp5_failure(SP5_REASON_PLATFORM);
        if (available)
            return sp5_success();
        if (attempt + 1 != SP5_PS5_CAPTURE_RESOURCE_QUERIES && !delay_us(SP5_PS5_CAPTURE_DELAY_US))
            return sp5_failure(SP5_REASON_PLATFORM);
    }
    return sp5_failure(SP5_REASON_TIMEOUT);
}

static void query_parameters(sp5_ps5_capture *capture)
{
    /* Probe order: request build/format, configuration-header query, then the
     * mode/capture-state open loop (capture_encode.c:663-671). A failed query
     * leaves parameter_size zero; frames still stream without in-band sets. */
    uint8_t packet[SP5_PS5_CAPTURE_PARAMETER_PACKET_BYTES] = {0};
    uint64_t packet_size = sizeof(packet);
    memcpy(packet, &packet_size, sizeof(packet_size));
    if (capture->api.query_headers(capture->request + 8, packet) != 0)
        return;
    capture->parameter_size = sp5_ps5_parameter_sets_parse(
        capture->parameters, sizeof(capture->parameters), packet, sizeof(packet), capture->codec);
}

static sp5_result video_open(sp5_ps5_capture *capture)
{
    if (!request_build(capture))
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    query_parameters(capture);
    int rc;
    bool opened = false;
    for (unsigned attempt = 0; attempt < SP5_PS5_CAPTURE_QUERY_ATTEMPTS; ++attempt) {
        uint32_t flags = 0;
        int query = capture->api.query_mode_flags(&flags);
        if (query < 0)
            break;
        memset(capture->capture_state, 0, sizeof(capture->capture_state));
        query = capture->api.query_capture_state(capture->capture_state);
        if (query < 0)
            break;
        capture->mode_flags = flags;
        request_apply_state(capture);
        if (capture->capture_state[0x14] != 0) {
            if (attempt + 1 == SP5_PS5_CAPTURE_QUERY_ATTEMPTS ||
                !delay_us(SP5_PS5_CAPTURE_DELAY_US))
                break;
            continue;
        }
        rc = capture->api.open(&capture->handle, capture->request, 0);
        if (rc >= 0) {
            opened = capture->handle != NULL;
            break;
        }
        /* On 0x81950008 ownership of a returned nonnull value is not established. */
        if ((uint32_t)rc != SP5_AVCAP2_STATUS_RETRY || capture->handle != NULL)
            break;
        if (attempt + 1 == SP5_PS5_CAPTURE_QUERY_ATTEMPTS || !delay_us(SP5_PS5_CAPTURE_DELAY_US))
            break;
    }
    if (!opened) {
        /* A non-null handle returned with failure is not proven owned, matching
         * the probe's cleanup_failed flag; it is not passed to Close. */
        if (capture->handle != NULL) {
            capture->cleanup_failure = SP5_REASON_PLATFORM;
            capture->cleanup_gate_failed = true;
            capture->failed_open_unproven = true;
        }
        capture->handle = NULL;
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    capture->opened = true;
    rc = capture->api.start(capture->handle);
    if (rc != 0)
        return sp5_failure(SP5_REASON_PLATFORM);
    capture->started = true;
    return sp5_success();
}

static sp5_result capture_close_state(sp5_ps5_capture *capture)
{
    sp5_result first = sp5_success();
    capture->parameter_size = 0;
    if (capture->leased) {
        capture->leased = false;
        capture->live_lease = 0;
        capture->surface.data = NULL;
        capture->surface.size = 0;
    }
    /* The first close after a failed open with an unproven non-null handle keeps
     * the probe's cleanup gate; the next close may unload after the retry. */
    bool local_ok = !capture->failed_open_unproven;
    capture->failed_open_unproven = false;
    if (capture->started) {
        int rc = capture->api.stop(capture->handle);
        /* Probe behavior: Close still follows and Stop is not retried after a
         * failure; the sticky cleanup gate preserves the failure. */
        capture->started = false;
        if (rc != 0) {
            if (first.status == SP5_OK)
                first = sp5_failure(SP5_REASON_PLATFORM);
            capture->cleanup_failure = SP5_REASON_PLATFORM;
            local_ok = false;
        }
    }
    if (capture->opened) {
        int rc = capture->api.close(capture->handle);
        /* Probe behavior: the returned handle is dropped even on close failure. */
        capture->opened = false;
        capture->started = false;
        capture->handle = NULL;
        if (rc != 0) {
            if (first.status == SP5_OK)
                first = sp5_failure(SP5_REASON_PLATFORM);
            capture->cleanup_failure = SP5_REASON_PLATFORM;
            local_ok = false;
        }
    }
    if (capture->requested) {
        int rc = capture->resource.request_state(SP5_PS5_CAPTURE_RESOURCE_ID, "STOP", capture,
                                                 (int32_t)getpid());
        capture->requested = false;
        if (rc != 0) {
            if (first.status == SP5_OK)
                first = sp5_failure(SP5_REASON_PLATFORM);
            capture->cleanup_failure = SP5_REASON_PLATFORM;
            local_ok = false;
        }
    }
    if (capture->registered) {
        int rc = capture->resource.unregister_client(SP5_PS5_CAPTURE_RESOURCE_ID);
        capture->registered = false;
        if (rc == 0)
            capture->unregistered = true;
        else {
            if (first.status == SP5_OK)
                first = sp5_failure(SP5_REASON_PLATFORM);
            capture->cleanup_failure = SP5_REASON_PLATFORM;
            local_ok = false;
        }
    }
    if (local_ok && capture->unregistered && !capture->selector_removed) {
        bool available = true;
        bool queried = false;
        for (unsigned attempt = 0; attempt < SP5_PS5_CAPTURE_CLEANUP_QUERIES; ++attempt) {
            queried = resource_query(capture, &available);
            if (!queried || !available) {
                capture->selector_removed = queried && !available;
                break;
            }
            if (attempt + 1 != SP5_PS5_CAPTURE_CLEANUP_QUERIES &&
                !delay_us(SP5_PS5_CAPTURE_DELAY_US))
                break;
        }
        if (!capture->selector_removed) {
            if (first.status == SP5_OK)
                first = sp5_failure(SP5_REASON_OWNERSHIP);
            capture->cleanup_failure = SP5_REASON_OWNERSHIP;
            local_ok = false;
        }
    }
    if (!local_ok)
        capture->cleanup_gate_failed = true;
    if (capture->global_acquired) {
        sp5_ps5_media_global_note_cleanup(local_ok);
        sp5_result released = sp5_ps5_media_global_release();
        if (released.status != SP5_OK) {
            if (first.status == SP5_OK) {
                first = released;
                if (capture->cleanup_failure == SP5_REASON_NONE)
                    capture->cleanup_failure = released.reason;
            }
        } else {
            capture->global_acquired = false;
        }
    }
    if (first.status == SP5_OK && !capture->global_acquired) {
        capture->closed = true;
        capture->cleanup_failure = SP5_REASON_NONE;
        capture->cleanup_gate_failed = false;
    }
    return first;
}

static sp5_result capture_open(void *context, const sp5_media_config *config)
{
    sp5_ps5_capture *capture = context;
    if (capture == NULL || config == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (capture->opened || capture->global_acquired || capture->leased)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    sp5_ps5_mode mode;
    if (!sp5_ps5_mode_validate(config, &mode))
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    if (mode.ceiling_kbps != 0 && config->chosen_bitrate_kbps > mode.ceiling_kbps)
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    if (!sp5_ps5_firmware_supported())
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    memset(capture->request, 0, sizeof(capture->request));
    memset(capture->info, 0, sizeof(capture->info));
    memset(capture->capture_state, 0, sizeof(capture->capture_state));
    capture->parameter_size = 0;
    capture->tier = mode.tier;
    capture->fps = mode.fps;
    capture->codec = mode.codec;
    capture->depth = mode.depth;
    capture->width = config->width;
    capture->height = config->height;
    capture->chosen_bitrate_kbps = config->chosen_bitrate_kbps;
    capture->dynamic_range = config->dynamic_range;
    capture->protection = SP5_PROTECTION_UNKNOWN;
    capture->protection_evidence_ref = NULL;
    capture->protection_observed = false;
    capture->source_epoch = 1;
    capture->next_lease = 0;
    capture->live_lease = 0;
    capture->next_frame_id = 0;
    capture->leased = false;
    capture->cleanup_failure = SP5_REASON_NONE;
    capture->cleanup_gate_failed = false;
    capture->closed = false;
    capture->started = false;
    capture->opened = false;
    capture->ever_registered = false;
    capture->registered = false;
    capture->requested = false;
    capture->unregistered = false;
    capture->selector_removed = false;
    memset(&capture->surface, 0, sizeof(capture->surface));
    memset(&capture->api, 0, sizeof(capture->api));
    memset(&capture->resource, 0, sizeof(capture->resource));
    sp5_result result = sp5_ps5_media_global_acquire();
    if (result.status != SP5_OK)
        return result;
    capture->global_acquired = true;
    const sp5_ps5_media_global_info *global = sp5_ps5_media_global_status();
    if (global == NULL || global->handle[SP5_PS5_MEDIA_GLOBAL_AVCAP2] < 0 ||
        global->handle[SP5_PS5_MEDIA_GLOBAL_RESOURCE] < 0) {
        result = sp5_failure(SP5_REASON_UNSUPPORTED);
        goto unwind;
    }
    if (!resolve_api(capture, (uint32_t)global->handle[SP5_PS5_MEDIA_GLOBAL_AVCAP2]) ||
        !resolve_resource(capture, (uint32_t)global->handle[SP5_PS5_MEDIA_GLOBAL_RESOURCE])) {
        result = sp5_failure(SP5_REASON_UNSUPPORTED);
        goto unwind;
    }
    result = resource_start(capture);
    if (result.status != SP5_OK)
        goto unwind;
    result = video_open(capture);
    if (result.status != SP5_OK)
        goto unwind;
    return sp5_success();

unwind: {
    sp5_result cleanup = capture_close_state(capture);
    (void)cleanup; /* Root failure is preserved; cleanup_diagnostic records the rest. */
}
    return result;
}

static sp5_result capture_source_state(void *context, sp5_source_state *state)
{
    sp5_ps5_capture *capture = context;
    if (capture == NULL || state == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *state = (sp5_source_state){0};
    if (!capture->opened)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    state->epoch = capture->source_epoch;
    state->protection = capture->protection;
    /* HDR requests are refused until the Avcap2 request/format mapping is
     * sourced for HDR; never report an unverified HDR range as observed. */
    state->dynamic_range = capture->dynamic_range == SP5_SDR ? SP5_SDR : SP5_DYNAMIC_RANGE_UNKNOWN;
    state->width = capture->width;
    state->height = capture->height;
    state->bit_depth = (uint8_t)(capture->dynamic_range == SP5_SDR ? capture->depth : 0);
    state->hdr = (sp5_hdr_metadata){
        .validity = SP5_VALIDITY_UNAVAILABLE,
        .reason = SP5_REASON_STAGE_UNAVAILABLE,
        .format_ref = "ps5/avcap2/read-info-v1 (opaque 0x70-byte record)",
        .source_evidence_ref =
            "specs/001-moonlight-streaming-host/research-r3-findings.md"
            "#t041-dispatch-9--retained-hevc10-pictures-and-independent-decode-2026-10-31",
        .payload = {0}};
    sp5_timestamp now;
    uint64_t microseconds;
    if (sp5_ps5_clock_now(&now, &microseconds).status == SP5_OK) {
        (void)microseconds;
        state->observed_at = now;
    } else {
        state->observed_at = sp5_ps5_clock_unavailable(SP5_REASON_CLOCK);
    }
    return sp5_success();
}

static sp5_result capture_acquire(void *context, sp5_timestamp deadline, bool metrics,
                                  sp5_video_frame *frame)
{
    sp5_ps5_capture *capture = context;
    if (capture == NULL || frame == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *frame = (sp5_video_frame){0};
    if (!capture->opened || !capture->started)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    if (capture->protection != SP5_PROTECTION_CLEAR) {
        return sp5_failure(capture->protection == SP5_PROTECTION_ACTIVE
                               ? SP5_REASON_PROTECTED
                               : SP5_REASON_PROTECTION_UNKNOWN);
    }
    if (capture->leased)
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    sp5_result result = sp5_ps5_clock_check_deadline(deadline);
    if (result.status != SP5_OK)
        return result;
    sp5_timestamp start;
    uint64_t start_us;
    result = sp5_ps5_clock_now(&start, &start_us);
    if (result.status != SP5_OK)
        return result;
    (void)start_us;
    memset(capture->info, 0, sizeof(capture->info));
    store64(capture->info, 0, 0x70);
    /* Flags 0 are the sourced caller form (blocking). The individual service
     * call deadline was not recovered; the adapter checks the deadline before
     * and after and never retries the record. */
    int rc = capture->api.read(capture->handle, capture->read_buffer, sizeof(capture->read_buffer),
                               capture->info, 0);
    sp5_timestamp end;
    uint64_t end_us;
    result = sp5_ps5_clock_now(&end, &end_us);
    if (result.status != SP5_OK)
        return result;
    (void)end_us;
    if (rc == 0 || (uint32_t)rc == SP5_AVCAP2_EMPTY_1 || (uint32_t)rc == SP5_AVCAP2_EMPTY_2)
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    if ((uint32_t)rc == SP5_AVCAP2_OVERRUN)
        return sp5_failure(SP5_REASON_COVERAGE_GAP);
    if (rc < 0 || (size_t)rc > sizeof(capture->read_buffer) ||
        load32(capture->info, 0x5c) != (uint32_t)rc) {
        capture->last_read_status = rc;
        capture->last_read_size = load32(capture->info, 0x5c);
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    size_t size = (size_t)rc;
    sp5_ps5_annexb_class classification =
        sp5_ps5_annexb_classify(capture->read_buffer, size, capture->codec);
    if (classification == SP5_PS5_ANNEXB_INVALID) {
        capture->last_read_status = rc;
        capture->last_read_size = UINT32_MAX; /* marks an unparseable record */
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    if (end.ticks > deadline.ticks)
        return sp5_failure(SP5_REASON_TIMEOUT); /* Record is dropped before any lease. */
    const char *format = sp5_ps5_format_ref(capture->codec, capture->depth);
    if (format == NULL)
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    if (capture->next_frame_id == UINT64_MAX || capture->next_lease == UINT64_MAX)
        return sp5_failure(SP5_REASON_OVERFLOW);
    uint64_t frame_id = ++capture->next_frame_id;
    uint64_t lease_id = ++capture->next_lease;
    capture->surface = (sp5_ps5_capture_surface){.magic = SP5_PS5_CAPTURE_SURFACE_MAGIC,
                                                 .version = SP5_PS5_CAPTURE_SURFACE_VERSION,
                                                 .data = capture->read_buffer,
                                                 .size = size,
                                                 .codec = capture->codec,
                                                 .key_frame = classification == SP5_PS5_ANNEXB_KEY,
                                                 .width = capture->width,
                                                 .height = capture->height,
                                                 .depth = capture->depth,
                                                 .frame_id = frame_id};
    capture->live_lease = lease_id;
    capture->leased = true;
    *frame = (sp5_video_frame){
        .lease_id = lease_id,
        .frame_id = frame_id,
        .source_epoch = capture->source_epoch,
        .surface = &capture->surface,
        .format_ref = format,
        .capture_timestamp = end,
        .capture_span = metrics ? (sp5_stage_span){start, end, SP5_VALIDITY_VALID, SP5_REASON_NONE}
                                : (sp5_stage_span){.reason = SP5_REASON_METRICS_DISABLED}};
    return sp5_success();
}

static sp5_result capture_release(void *context, uint64_t lease_id)
{
    sp5_ps5_capture *capture = context;
    if (capture == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (!capture->leased || lease_id != capture->live_lease)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    capture->leased = false;
    capture->live_lease = 0;
    capture->surface.data = NULL;
    capture->surface.size = 0;
    return sp5_success();
}

static sp5_result capture_close(void *context)
{
    sp5_ps5_capture *capture = context;
    if (capture == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (capture->closed && !capture->opened && !capture->global_acquired && !capture->registered &&
        !capture->requested && !capture->leased)
        return sp5_success();
    return capture_close_state(capture);
}

sp5_result sp5_ps5_capture_restart(sp5_ps5_capture *capture)
{
    if (capture == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    /* The session's resource and media global must be held; the video stream
     * itself may already be closed by an earlier restart that failed to open. */
    if (!capture->global_acquired || !capture->requested || capture->leased)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    if (capture->started) {
        if (capture->api.stop(capture->handle) != 0)
            capture->cleanup_failure = SP5_REASON_PLATFORM;
        capture->started = false;
    }
    if (capture->opened) {
        /* As in capture_close_state, the handle is dropped even on close failure. */
        if (capture->api.close(capture->handle) != 0)
            capture->cleanup_failure = SP5_REASON_PLATFORM;
        capture->opened = false;
        capture->handle = NULL;
    }
    return video_open(capture);
}

void sp5_ps5_capture_init(sp5_ps5_capture *capture)
{
    if (capture == NULL)
        return;
    memset(capture, 0, sizeof(*capture));
    capture->protection = SP5_PROTECTION_UNKNOWN;
}

sp5_capture_ops sp5_ps5_capture_operations(sp5_ps5_capture *capture)
{
    return (sp5_capture_ops){.context = capture,
                             .open = capture_open,
                             .source_state = capture_source_state,
                             .acquire = capture_acquire,
                             .release = capture_release,
                             .close = capture_close};
}

void sp5_ps5_capture_provenance(const sp5_ps5_capture *capture, sp5_platform_provenance *provenance)
{
    (void)capture;
    if (provenance == NULL)
        return;
    *provenance = (sp5_platform_provenance){
        .synthetic = false,
        .implementation_ref = "platform/capture/ps5.c (provisional; compile-only until a "
                              "production Avcap2 run is retained)",
        .firmware = "13.60 (13.600.007)",
        .capability_evidence_ref = "specs/001-moonlight-streaming-host/research-r3-findings.md"
                                   "#t041-required-evidence-ledger",
        .validity = SP5_VALIDITY_INVALID,
        .reason = SP5_REASON_UNVERIFIED};
}

bool sp5_ps5_capture_parameters(const sp5_ps5_capture *capture, const uint8_t **data, size_t *size)
{
    if (data != NULL)
        *data = NULL;
    if (size != NULL)
        *size = 0;
    if (capture == NULL || data == NULL || size == NULL || capture->parameter_size == 0 ||
        capture->parameter_size > sizeof(capture->parameters))
        return false;
    *data = capture->parameters;
    *size = capture->parameter_size;
    return true;
}

sp5_result sp5_ps5_capture_observe_protection(sp5_ps5_capture *capture, sp5_protection protection,
                                              const char *evidence_ref)
{
    if (capture == NULL || protection < SP5_PROTECTION_UNKNOWN ||
        protection > SP5_PROTECTION_ACTIVE)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (!capture->opened)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    if (protection == SP5_PROTECTION_CLEAR && (evidence_ref == NULL || evidence_ref[0] == 0))
        return sp5_failure(SP5_REASON_UNVERIFIED);
    if (capture->protection_observed && capture->protection == protection) {
        capture->protection_evidence_ref = evidence_ref;
        return sp5_success(); /* Same state: epoch does not advance. */
    }
    if (capture->source_epoch == UINT64_MAX)
        return sp5_failure(SP5_REASON_OVERFLOW);
    ++capture->source_epoch; /* First observation or a real protection change. */
    capture->protection = protection;
    capture->protection_evidence_ref = evidence_ref;
    capture->protection_observed = true;
    return sp5_success();
}

// NOLINTEND(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
