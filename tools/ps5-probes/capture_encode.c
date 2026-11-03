/* Nonshipping R3/T041 candidate probes, NOT a production capture/encoder implementation.
 * ABI: ps5-payload-dev/sdk a7685ce8e97ce1f8a4f38253a41d54241c9124f1,
 * include/ps5/kernel.h, crt/kernel.c (firmware helper and 13.60 mask),
 * target/include/sys/stat.h. Module names: crt/rtld_sprx.c and PSCapture
 * d292d0fbf7434d1aeda50103911e4d2a18266b85 docs/REVERSE_ENGINEERING.md.
 * stat() checks candidate paths only; it neither loads a module nor proves ABI,
 * permissions, visibility in another namespace, or any media capability.
 * Target-only modes make bounded Avcap2 calls; the probe adds no kernel memory
 * operations/hooks and normally discards picture/audio payloads. Explicit
 * R3_MEDIA_SAVE_VIDEO retains bounded video records for post-cleanup export.
 * Bounded codec parameter
 * and SEI NALs can be printed privately for independent format inspection.
 * Stock SDK crt/crt.c -> crt/patch.c changes process credentials/syscall bounds
 * before main; its reversal is unverified. Do not claim zero kernel writes.
 */
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef R3_HOST
#include <ps5/kernel.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

#include "media_probe.h"

#if defined(R3_MEDIA_SAVE_VIDEO) && (!defined(R3_MEDIA_VIDEO) || !defined(R3_MEDIA_HEADERS))
#error R3_MEDIA_SAVE_VIDEO requires R3_MEDIA_VIDEO and R3_MEDIA_HEADERS
#endif

#if !defined(R3_HOST) || defined(R3_MEDIA_TEST)
R3_MEDIA_LOCAL bool media_copy(void *destination, size_t capacity, const void *source, size_t count)
{
    if (count > capacity || (count != 0 && (!destination || !source)))
        return false;
    unsigned char *output = destination;
    const unsigned char *input = source;
    for (size_t i = 0; i < count; ++i)
        output[i] = input[i];
    return true;
}

R3_MEDIA_LOCAL bool media_bind_pointer(void *destination, size_t capacity, uintptr_t address)
{
    /* The sourced SDK resolver supplies an address, not a callable stub.
     * Copy its representation only on the observed flat 64-bit function-pointer
     * ABI; incompatible representations and null exports are refused. */
    return address != 0 && capacity == sizeof(address) &&
           media_copy(destination, capacity, &address, sizeof(address));
}

R3_MEDIA_LOCAL bool media_modules_cleanup(const struct media_module_api *api,
                                          struct media_modules *state, bool prerequisites_ok)
{
    state->failed |= !prerequisites_ok || state->count > R3_MEDIA_MODULE_CAPACITY || !api ||
                     !api->unload || !api->observe;
    if (state->failed)
        return false;
    for (size_t index = state->count; index > 0; --index) {
        struct media_module *module = &state->entries[index - 1];
        if (!module->owned)
            continue;
        if (module->attempted) {
            if (!module->completed || module->rc != 0 || module->result != 0) {
                state->failed = true;
                return false;
            }
            continue;
        }
        if (!module->name || module->handle < 0) {
            state->failed = true;
            return false;
        }
        /* Claim the attempt before logging or calling into a module. Re-entry
         * after interruption cannot retry an operation of unknown outcome. */
        module->attempted = true;
        if (!api->observe(api->context, module)) {
            state->failed = true;
            return false;
        }
        int result = 0;
        int rc = api->unload(api->context, module->handle, &result);
        module->rc = rc;
        module->result = result;
        module->completed = true;
        bool observed = api->observe(api->context, module);
        state->failed |= rc != 0 || result != 0 || !observed;
        if (state->failed)
            return false;
    }
    return true;
}

R3_MEDIA_LOCAL bool audio_start(const struct audio_api *api, struct audio_state *s)
{
    int rc = api->initialize();
    printf("R3_AUDIO_CALL operation=initialize rc=0x%08x\n", (unsigned)rc);
    if (rc != 0)
        return false;
    s->initialized = true;
    const struct audio_request request = {
        .size = 0x38, .unknown08 = 0x18, .source = 2, .unknown1c = UINT32_MAX};
    rc = api->open(&s->handle, &request, 0);
    printf("R3_AUDIO_CALL operation=open rc=0x%08x handle_present=%s\n", (unsigned)rc,
           s->handle ? "yes" : "no");
    if (rc != 0) {
        /* Failure does not establish ownership of any returned nonnull value. */
        s->cleanup_failed = s->handle != NULL;
        s->handle = NULL;
        return false;
    }
    if (!s->handle)
        return false;
    rc = api->start(s->handle);
    printf("R3_AUDIO_CALL operation=start rc=0x%08x\n", (unsigned)rc);
    s->started = rc == 0;
    return s->started;
}

R3_MEDIA_LOCAL bool audio_read(const struct audio_api *api, struct audio_state *s)
{
    if (!s->started)
        return false;
    for (size_t i = 0; i < sizeof(s->metadata); ++i)
        s->metadata[i] = 0;
    s->metadata[0] = 0x10;
    int rc = api->read(s->handle, s->samples, sizeof(s->samples), s->metadata, 1);
    ++s->reads;
    if (rc == 0 || (uint32_t)rc == UINT32_C(0x81950002)) {
        ++s->empty;
        return true;
    }
    if ((uint32_t)rc == UINT32_C(0x81950004)) {
        ++s->overruns;
        return true;
    }
    if (rc < 0 || (size_t)rc > sizeof(s->samples) || rc % 8 != 0) {
        printf("R3_AUDIO_READ status=invalid rc=0x%08x\n", (unsigned)rc);
        return false;
    }
    size_t count = (size_t)rc / sizeof(float);
    for (size_t i = 0; i < count; ++i) {
        /* Avoid the target sysroot's qualified _Generic math macros. */
        if (!(s->samples[i] >= -FLT_MAX && s->samples[i] <= FLT_MAX)) {
            puts("R3_AUDIO_READ status=invalid reason=nonfinite_sample");
            return false;
        }
    }
    /* Count a record only after the entire returned record has validated. */
    for (size_t i = 0; i < count; ++i) {
        size_t channel = i % 2;
        float magnitude = fabsf(s->samples[i]);
        if (magnitude > s->peak[channel])
            s->peak[channel] = magnitude;
        if (magnitude != 0)
            ++s->nonzero[channel];
    }
    s->bytes += (uint64_t)rc;
    s->frames += count / 2;
    return true;
}

R3_MEDIA_LOCAL bool audio_cleanup(const struct audio_api *api, struct audio_state *s)
{
    if (s->started) {
        int rc = api->stop(s->handle);
        printf("R3_AUDIO_CLEANUP operation=stop rc=0x%08x\n", (unsigned)rc);
        s->cleanup_failed |= rc != 0;
        s->started = false;
    }
    if (s->handle) {
        int rc = api->close(s->handle);
        printf("R3_AUDIO_CLEANUP operation=close rc=0x%08x\n", (unsigned)rc);
        s->cleanup_failed |= rc != 0;
        s->handle = NULL;
    }
    if (s->initialized) {
        int rc = api->terminate();
        printf("R3_AUDIO_CLEANUP operation=terminate rc=0x%08x\n", (unsigned)rc);
        s->cleanup_failed |= rc != 0;
        s->initialized = false;
    }
    return !s->cleanup_failed;
}
#endif

#if !defined(R3_HOST) || defined(R3_MEDIA_TEST)
R3_MEDIA_LOCAL bool resource_start(const struct resource_api *api, struct resource_state *s,
                                   resource_callback callback)
{
    int rc = api->register_client(R3_VIDEO_RESOURCE_ID, callback, s);
    printf("R3_RESOURCE_CALL operation=register id=0xb4 rc=0x%08x\n", (unsigned)rc);
    s->registered = rc >= 0;
    if (rc != 0)
        return false;
    s->requested = true;
    rc = api->request_state(R3_VIDEO_RESOURCE_ID, "RUNNING", s, s->pid);
    printf("R3_RESOURCE_CALL operation=request_running rc=0x%08x\n", (unsigned)rc);
    return rc == 0;
}

R3_MEDIA_LOCAL bool resource_query(const struct resource_api *api, struct resource_state *s)
{
    s->available = false;
    for (size_t i = 0; i < R3_RESOURCE_SELECTORS; ++i)
        s->selectors[i] = INT32_MIN;
    int rc = api->query_selectors(s->selectors);
    ++s->queries;
    if (rc != 0) {
        printf("R3_RESOURCE_CALL operation=query rc=0x%08x\n", (unsigned)rc);
        return false;
    }
    for (size_t i = 0; i < R3_RESOURCE_SELECTORS; ++i)
        s->available |= s->selectors[i] == R3_VIDEO_RESOURCE_ID;
    return true;
}

R3_MEDIA_LOCAL bool resource_cleanup(const struct resource_api *api, struct resource_state *s)
{
    if (s->requested) {
        int rc = api->request_state(R3_VIDEO_RESOURCE_ID, "STOP", s, s->pid);
        printf("R3_RESOURCE_CLEANUP operation=request_stop rc=0x%08x\n", (unsigned)rc);
        s->cleanup_failed |= rc != 0;
        s->requested = false;
    }
    if (s->registered) {
        int rc = api->unregister_client(R3_VIDEO_RESOURCE_ID);
        printf("R3_RESOURCE_CLEANUP operation=unregister rc=0x%08x\n", (unsigned)rc);
        s->cleanup_failed |= rc != 0;
        s->registered = false;
    }
    return !s->cleanup_failed;
}

R3_MEDIA_LOCAL bool resource_wait_removed(const struct resource_api *api, struct resource_state *s,
                                          bool (*wait_next)(void *), void *context)
{
    if (s->cleanup_failed)
        return false;
    for (unsigned i = 0; i < R3_RESOURCE_CLEANUP_QUERIES; ++i) {
        if (!resource_query(api, s))
            break;
        if (!s->available)
            return true;
        if (i + 1 == R3_RESOURCE_CLEANUP_QUERIES || !wait_next(context))
            break;
    }
    s->cleanup_failed = true;
    return false;
}
#endif

#if !defined(R3_HOST) || defined(R3_MEDIA_TEST)
static void video_store32(struct video_request *request, size_t offset, uint32_t value)
{
    if (offset <= sizeof(request->bytes) - sizeof(value))
        (void)media_copy(request->bytes + offset, sizeof(request->bytes) - offset, &value,
                         sizeof(value));
}

static void video_store64(struct video_request *request, size_t offset, uint64_t value)
{
    if (offset <= sizeof(request->bytes) - sizeof(value))
        (void)media_copy(request->bytes + offset, sizeof(request->bytes) - offset, &value,
                         sizeof(value));
}

static uint32_t video_load32(const unsigned char *bytes, size_t offset)
{
    uint32_t value;
    (void)media_copy(&value, sizeof(value), bytes + offset, sizeof(value));
    return value;
}

R3_MEDIA_LOCAL bool video_request_init(struct video_request *request, unsigned tier, unsigned fps,
                                       uint32_t bitrate_kbps)
{
    if ((tier != 3 && tier != 4) || (fps != 30 && fps != 60) || bitrate_kbps == 0 ||
        bitrate_kbps > INT32_MAX / 1000)
        return false;
    *request = (struct video_request){0};
    /* Venc 0x811408920 derives these fields from level bit limits with >>3;
     * 0x5baf0c53/0x5baf4320 copy our fields directly into that configuration.
     * SceRemotePlay 0x41c081bb-0x41c08226 writes +0xcc as kbps * 1000, so the
     * request carries bits/s and bits/frame. The earlier 125 factor was
     * bytes/s and under-requested by 8x (measured 2.85 Mbps at 20 Mbps). */
    uint64_t bitrate = (uint64_t)bitrate_kbps * 1000;
    /* SceRemotePlay 0x41c27840: request starts at context+0x40; the
     * embedded header is 0x2e8/0x28 bytes, source is 2 and selector is
     * context+0x2003ec (resource 0xb4), at request+0x28. */
    video_store64(request, 0x00, 0x2e8);
    video_store64(request, 0x08, 0x28);
    video_store32(request, 0x14, 2);
    video_store32(request, 0x28, R3_VIDEO_RESOURCE_ID);
    video_store32(request, 0x18, tier);
    video_store32(request, 0x1c, fps != 30);
    /* 0x41c28190 receives the whole context, so its offsets are translated
     * by the request base at context+0x40. */
    video_store32(request, 0x98, 1);
    video_store32(request, 0xc8, (uint32_t)((float)bitrate / 1.12f));
    video_store32(request, 0xcc, (uint32_t)bitrate);
    video_store32(request, 0xd0, (uint32_t)(bitrate * 1001 / ((uint64_t)fps * 1000)));
    video_store32(request, 0x190, 1);
    video_store32(request, 0x194, 1);
    video_store64(request, 0x1e0, 0x88);
    video_store64(request, 0x1e8, UINT64_C(0x3ff0000000000000));
    video_store32(request, 0x20c, 384);
    video_store32(request, 0x210, 384);
    video_store32(request, 0x214, 512);
    video_store64(request, 0x218, 0x48000);
    return true;
}

R3_MEDIA_LOCAL bool video_request_set_format(struct video_request *request, unsigned codec,
                                             unsigned depth)
{
    /* SceRemotePlay 0x41c279d1..0x41c279f9; SceAvCapture 0x5baf3e30
     * maps request+0x20 to depth 8/10 and +0x24 to codec 1/2. Venc
     * 0x811407aa6 rejects depth 10 for codec 1 (AVC). */
    if (codec > 1 || (depth != 8 && depth != 10) || (codec == 0 && depth == 10))
        return false;
    video_store32(request, 0x20, depth == 10);
    video_store32(request, 0x24, codec);
    uint32_t peak = video_load32(request->bytes, 0xcc);
    video_store32(request, 0xc8, (uint32_t)((float)peak / (codec == 0 ? 1.12f : 1.10f)));
    return true;
}

static size_t video_start_code(const unsigned char *bytes, size_t size, size_t offset)
{
    if (offset > size || size - offset < 3 || bytes[offset] != 0 || bytes[offset + 1] != 0)
        return 0;
    if (bytes[offset + 2] == 1)
        return 3;
    return size - offset >= 4 && bytes[offset + 2] == 0 && bytes[offset + 3] == 1 ? 4 : 0;
}

R3_MEDIA_LOCAL bool video_record(struct video_recording *saved, const unsigned char *bytes,
                                 size_t size, unsigned codec)
{
    if (!saved)
        return true;
    if (saved->invalid)
        return false;
    if (!bytes || !size || size > R3_VIDEO_BUFFER_BYTES || codec > 1) {
        saved->invalid = true;
        return false;
    }
    /* Validate every Annex-B boundary/header; slice completeness still requires
     * independent decoding. Never normalize or omit bytes from a read record. */
    for (size_t offset = 0; offset < size;) {
        size_t prefix = video_start_code(bytes, size, offset);
        size_t payload = offset + prefix;
        size_t header = codec == 0 ? 1 : 2;
        if (!prefix || size - payload <= header || (bytes[payload] & 0x80) ||
            (codec == 1 && (bytes[payload + 1] & 7) == 0) ||
            (codec == 0 && (bytes[payload] & 31) == 0)) {
            saved->invalid = true;
            return false;
        }
        offset = payload + header;
        while (offset < size && !video_start_code(bytes, size, offset))
            ++offset;
    }
    if (saved->used > R3_VIDEO_SAVED_BYTES || saved->count >= R3_VIDEO_SAVED_RECORDS ||
        size > R3_VIDEO_SAVED_BYTES - saved->used) {
        saved->invalid = saved->overflow = true;
        return false;
    }
    if (!media_copy(saved->bytes + saved->used, R3_VIDEO_SAVED_BYTES - saved->used, bytes, size)) {
        saved->invalid = true;
        return false;
    }
    saved->length[saved->count++] = size;
    saved->used += size;
    return true;
}

/* The public target libc has no Annex K snprintf_s. Every snprintf below has
 * a fixed format, scalar arguments, an explicit bound and checked truncation. */
// NOLINTBEGIN(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
R3_MEDIA_LOCAL bool video_export(FILE *output, const struct video_recording *saved, bool complete)
{
    if (!saved)
        return true;
    if (!output)
        return false;
    bool valid = complete && !saved->invalid && !saved->overflow && saved->count != 0 &&
                 saved->count <= R3_VIDEO_SAVED_RECORDS && saved->used <= R3_VIDEO_SAVED_BYTES;
    size_t total = 0;
    for (size_t i = 0; valid && i < saved->count; ++i) {
        valid = saved->length[i] != 0 && saved->length[i] <= saved->used - total;
        if (valid)
            total += saved->length[i];
    }
    valid = valid && total == saved->used;
    char line[256];
    int written =
        snprintf(line, sizeof(line),
                 "R3_VIDEO_EXPORT_BEGIN valid=%d records=%zu bytes=%zu overflow=%d "
                 "invalid=%d chunk_bytes=128\n",
                 (int)valid, saved->count, saved->used, (int)saved->overflow, (int)!valid);
    if (written < 0 || (size_t)written >= sizeof(line) || fputs(line, output) == EOF)
        return false;
    if (!valid)
        return false;
    size_t base = 0;
    for (size_t i = 0; i < saved->count; ++i) {
        for (size_t offset = 0; offset < saved->length[i];) {
            size_t length = saved->length[i] - offset;
            if (length > 128)
                length = 128;
            written =
                snprintf(line, sizeof(line),
                         "R3_VIDEO_DATA record=%zu record_bytes=%zu offset=%zu bytes=%zu hex=", i,
                         saved->length[i], offset, length);
            if (written < 0 || (size_t)written >= sizeof(line) || fputs(line, output) == EOF)
                return false;
            static const char hex[] = "0123456789abcdef";
            for (size_t j = 0; j < length; ++j) {
                unsigned byte = saved->bytes[base + offset + j];
                if (fputc(hex[byte >> 4], output) == EOF || fputc(hex[byte & 15], output) == EOF)
                    return false;
            }
            if (fputc('\n', output) == EOF)
                return false;
            offset += length;
        }
        base += saved->length[i];
    }
    /* Consumers require this terminal marker and all indexed chunks. A broken
     * transport or short output can never establish a complete recording. */
    written = snprintf(line, sizeof(line), "R3_VIDEO_EXPORT_END records=%zu bytes=%zu complete=1\n",
                       saved->count, saved->used);
    return written >= 0 && (size_t)written < sizeof(line) && fputs(line, output) != EOF &&
           fflush(output) != EOF && !ferror(output);
}
// NOLINTEND(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)

static bool video_hdr_sei(const unsigned char *bytes, size_t size, bool *present)
{
    /* FFmpeg n8.0 libavcodec/sei.h: mastering-display and content-light
     * payload types 137/144. Unescape before respecting payload byte counts;
     * unrelated UUID/timing messages must not consume the collection bound. */
    *present = false;
    if (size > R3_VIDEO_PARAMETER_BYTES)
        return false;
    unsigned char rbsp[R3_VIDEO_PARAMETER_BYTES];
    size_t used = 0;
    unsigned zeros = 0;
    for (size_t i = 0; i < size; ++i) {
        unsigned char value = bytes[i];
        if (zeros == 2 && value == 3) {
            if (i + 1 == size || bytes[i + 1] > 3)
                return false;
            zeros = 0;
            continue;
        }
        rbsp[used++] = value;
        zeros = value == 0 ? (zeros < 2 ? zeros + 1 : 2) : 0;
    }
    size_t offset = 0;
    while (offset < used) {
        if (rbsp[offset] == 0x80) {
            bool trailing = true;
            for (size_t i = offset + 1; i < used; ++i)
                trailing &= rbsp[i] == 0;
            if (trailing)
                return true;
        }
        size_t type = 0, length = 0;
        unsigned char value;
        do {
            if (offset == used)
                return false;
            value = rbsp[offset++];
            type += value;
        } while (value == 255);
        do {
            if (offset == used)
                return false;
            value = rbsp[offset++];
            length += value;
        } while (value == 255);
        if (length > used - offset)
            return false;
        if (type == 137 || type == 144) {
            if (length != (type == 137 ? 24U : 4U))
                return false;
            *present = true;
        }
        offset += length;
    }
    return false;
}

R3_MEDIA_LOCAL bool video_collect_parameters(struct video_parameters *parameters,
                                             const unsigned char *bytes, size_t size,
                                             unsigned codec)
{
    /* FFmpeg n8.0 libavcodec/{h264.h,hevc/hevc.h}, H.264/H.265 Table 7-1.
     * Preserve first parameter NAL per type and distinct HDR SEI NALs,
     * never VCL picture data.
     * Collection is bounded independently of the 2 MiB read buffer. */
    if (codec > 1 || size > R3_VIDEO_BUFFER_BYTES || parameters->overflow)
        return false;
    size_t offset = 0;
    while (offset < size) {
        size_t prefix = video_start_code(bytes, size, offset);
        if (prefix == 0) {
            ++offset;
            continue;
        }
        size_t payload = offset + prefix;
        size_t header = codec == 0 ? 1 : 2;
        if (size - payload < header)
            return false;
        unsigned type = codec == 0 ? bytes[payload] & 31U : (bytes[payload] >> 1) & 63U;
        size_t end = payload + header;
        while (end < size && video_start_code(bytes, size, end) == 0)
            ++end;
        bool metadata = codec == 0 ? type == 6 : type == 39 || type == 40;
        bool selected =
            codec == 0 ? type == 7 || type == 8 : type == 32 || type == 33 || type == 34;
        if (metadata) {
            if (end - offset > R3_VIDEO_PARAMETER_BYTES) {
                parameters->overflow = true;
                return false;
            }
            if (!video_hdr_sei(bytes + payload + header, end - payload - header, &selected))
                return false;
        }
        if (selected) {
            size_t length = end - offset;
            size_t index = 0;
            while (index < parameters->count) {
                if (parameters->type[index] == type &&
                    (!metadata || (length == parameters->length[index] &&
                                   memcmp(parameters->bytes + parameters->offset[index],
                                          bytes + offset, length) == 0)))
                    break;
                ++index;
            }
            if (index < parameters->count) {
                if (length != parameters->length[index] ||
                    memcmp(parameters->bytes + parameters->offset[index], bytes + offset, length) !=
                        0)
                    ++parameters->changes;
            } else {
                if (index >= sizeof(parameters->type) / sizeof(parameters->type[0]) ||
                    length > sizeof(parameters->bytes) - parameters->used) {
                    parameters->overflow = true;
                    return false;
                }
                parameters->type[index] = type;
                parameters->offset[index] = parameters->used;
                parameters->length[index] = length;
                if (!media_copy(parameters->bytes + parameters->used,
                                sizeof(parameters->bytes) - parameters->used, bytes + offset,
                                length))
                    return false;
                parameters->used += length;
                ++parameters->count;
            }
        }
        offset = end;
    }
    return true;
}

R3_MEDIA_LOCAL bool video_collect_header_packet(struct video_parameters *parameters,
                                                const unsigned char *packet, size_t size,
                                                unsigned codec)
{
    /* Sony caller 0x41c28ec0..0x41c290b3; Avcap2 0x814e494f0 /
     * 0x814e4c890 (IPC 0xa70008). SceAvCapture packers 0x5bae1f68
     * and 0x5bae2b3d strip four-byte prefixes into <=128-byte slots.
     * This is configuration-header signaling, not an observed VCL decode. */
    uint64_t packet_size;
    if (!parameters || !packet || size != 0x1a8 || codec > 1 ||
        !media_copy(&packet_size, sizeof(packet_size), packet, sizeof(packet_size)) ||
        packet_size != size || video_load32(packet, 8) != codec)
        return false;
    static const size_t lengths[] = {0x10, 0x98, 0x120};
    static const size_t payloads[] = {0x18, 0xa0, 0x128};
    static const unsigned avc_types[] = {7, 8};
    static const unsigned hevc_types[] = {32, 33, 34};
    unsigned count = codec == 0 ? 2 : 3;
    uint64_t sizes[3] = {0};
    for (unsigned i = 0; i < count; ++i) {
        unsigned slot = codec == 0 ? i : (i + 2) % 3;
        if (!media_copy(&sizes[i], sizeof(sizes[i]), packet + lengths[slot], sizeof(sizes[i])) ||
            sizes[i] < (codec == 0 ? 1U : 2U) || sizes[i] > 128)
            return false;
        unsigned type =
            codec == 0 ? packet[payloads[slot]] & 31U : (packet[payloads[slot]] >> 1) & 63U;
        if (type != (codec == 0 ? avc_types[i] : hevc_types[i]))
            return false;
    }
    struct video_parameters staged = *parameters;
    for (unsigned i = 0; i < count; ++i) {
        unsigned slot = codec == 0 ? i : (i + 2) % 3;
        unsigned char nal[132] = {0, 0, 0, 1};
        if (!media_copy(nal + 4, sizeof(nal) - 4, packet + payloads[slot], (size_t)sizes[i]) ||
            !video_collect_parameters(&staged, nal, (size_t)sizes[i] + 4, codec))
            return false;
    }
    *parameters = staged;
    return true;
}

static void video_request_apply_capture_state(struct video_request *request, uint32_t mode_flags,
                                              const unsigned char *capture_state)
{
    /* Remote Play's default constructor sets context+0x330 and the low
     * mode-class dword at context+0x2003e8 to 2. Its mode-2 table maps index 2
     * to 8; the caller chooses 6 for flag bit 0 and 8 for flag bit 1. */
    uint32_t selected_mode = 2;
    if ((mode_flags & 1U) != 0)
        selected_mode = 6;
    else if ((mode_flags & 2U) != 0)
        selected_mode = 8;
    video_store32(request, 0x2a8, mode_flags);
    video_store32(request, 0x2ac, selected_mode);
    request->bytes[0x2bc] = (unsigned char)(capture_state[0x17] ^ 1U);
}

static bool video_retry_pause(void)
{
#ifndef R3_HOST
    struct timespec delay = {.tv_nsec = 16000000};
    if (nanosleep(&delay, NULL) != 0)
        return false;
#endif
    return true;
}

R3_MEDIA_LOCAL bool video_start(const struct video_api *api, struct video_state *s)
{
    int rc = api->initialize();
    printf("R3_VIDEO_CALL operation=initialize rc=0x%08x\n", (unsigned)rc);
    if (rc != 0)
        return false;
    s->initialized = true;
    if (s->tier == 0) {
        s->tier = 3;
        s->fps = 30;
        s->bitrate_kbps = 10000;
    }
    struct video_request request;
    if (s->depth == 0)
        s->depth = 8;
    if (!video_request_init(&request, s->tier, s->fps, s->bitrate_kbps) ||
        !video_request_set_format(&request, s->codec, s->depth))
        return false;
    if (api->query_headers && s->checkpoint == R3_VIDEO_STREAM) {
        unsigned char packet[0x1a8] = {0};
        uint64_t packet_size = sizeof(packet);
        if (!media_copy(packet, sizeof(packet), &packet_size, sizeof(packet_size)))
            return false;
        rc = api->query_headers(request.bytes + 8, packet);
        bool valid = rc == 0 &&
                     video_collect_header_packet(&s->parameters, packet, sizeof(packet), s->codec);
        printf("R3_VIDEO_HEADER_QUERY rc=0x%08x valid=%d packet_bytes=%zu "
               "origin=configuration_query association_with_read_frames=unverified\n",
               (unsigned)rc, (int)valid, sizeof(packet));
        if (!valid)
            return false;
    }
    if (s->checkpoint == R3_VIDEO_INVALID_MODE) {
        /* Avcap2 0x814e4848a..0x814e484e2 rejects mode 3 locally,
         * before IPC 0xa70001, with 0x81950001. Owned initialization
         * and resource registration still require ordinary cleanup. */
        video_store32(&request, 0x1a8, 3);
    }
    if (!api->query_mode_flags || !api->query_capture_state) {
        puts("R3_VIDEO_REFUSED reason=missing_capture_state_query");
        return false;
    }
    bool opened = false;
    for (unsigned attempt = 0; attempt < R3_VIDEO_QUERY_ATTEMPTS; ++attempt) {
        uint32_t mode_flags = 0;
        int query_rc = api->query_mode_flags(&mode_flags);
        ++s->mode_flag_queries;
        printf("R3_VIDEO_CALL operation=query_mode_flags rc=0x%08x flags=0x%08x\n",
               (unsigned)query_rc, mode_flags);
        if (query_rc < 0)
            break;
        unsigned char capture_state[R3_VIDEO_CAPTURE_STATE_BYTES] = {0};
        query_rc = api->query_capture_state(capture_state);
        ++s->capture_state_queries;
        printf("R3_VIDEO_CALL operation=query_capture_state rc=0x%08x ready=0x%02x mode=0x%02x\n",
               (unsigned)query_rc, capture_state[0x14], capture_state[0x17]);
        if (query_rc < 0)
            break;
        video_request_apply_capture_state(&request, mode_flags, capture_state);
        if (capture_state[0x14] != 0) {
            if (attempt + 1 == R3_VIDEO_QUERY_ATTEMPTS || !video_retry_pause())
                break;
            continue;
        }
        ++s->open_attempts;
        rc = api->open(&s->handle, &request, 0);
        s->open_result = rc;
        printf("R3_VIDEO_CALL operation=open attempt=%u rc=0x%08x handle_present=%s\n",
               s->open_attempts, (unsigned)rc, s->handle ? "yes" : "no");
        if (rc >= 0) {
            opened = s->handle != NULL;
            break;
        }
        if ((uint32_t)rc != UINT32_C(0x81950008) || s->handle != NULL)
            break;
        if (attempt + 1 == R3_VIDEO_QUERY_ATTEMPTS || !video_retry_pause())
            break;
    }
    if (!opened) {
        s->cleanup_failed = s->handle != NULL;
        s->handle = NULL;
        return false;
    }
    if (!s->handle)
        return false;
    if (s->checkpoint == R3_VIDEO_OPEN_ONLY)
        return true;
    rc = api->start(s->handle);
    printf("R3_VIDEO_CALL operation=start rc=0x%08x\n", (unsigned)rc);
    s->started = rc == 0;
    return s->started;
}

R3_MEDIA_LOCAL bool video_read(const struct video_api *api, struct video_state *s)
{
    if (!s->started)
        return false;
    for (size_t i = 0; i < sizeof(s->info); ++i)
        s->info[i] = 0;
    s->info[0] = 0x70;
    int rc = api->read(s->handle, s->buffer, sizeof(s->buffer), s->info, 0);
    ++s->reads;
    if (rc == 0 || (uint32_t)rc == UINT32_C(0x81950002) || (uint32_t)rc == UINT32_C(0x8195000d)) {
        ++s->empty;
        return true;
    }
    if ((uint32_t)rc == UINT32_C(0x81950004)) {
        ++s->overruns;
        if (s->recording)
            s->recording->invalid = true;
        return true;
    }
    size_t size = rc < 0 ? 0 : (size_t)rc;
    if (rc < 0 || size > sizeof(s->buffer) || video_load32(s->info, 0x5c) != (uint32_t)rc) {
        printf("R3_VIDEO_READ status=invalid rc=0x%08x info_result=0x%08x\n", (unsigned)rc,
               video_load32(s->info, 0x5c));
        if (s->recording)
            s->recording->invalid = true;
        return false;
    }
    bool annexb = size >= 3 && s->buffer[0] == 0 && s->buffer[1] == 0 && s->buffer[2] == 1;
    annexb = annexb || (size >= 4 && s->buffer[0] == 0 && s->buffer[1] == 0 && s->buffer[2] == 0 &&
                        s->buffer[3] == 1);
    if (!annexb) {
        puts("R3_VIDEO_READ status=invalid reason=no_annexb_start_code");
        if (s->recording)
            s->recording->invalid = true;
        return false;
    }
    if (!video_record(s->recording, s->buffer, size, s->codec))
        return false;
    uint64_t nonzero = 0;
    for (size_t i = 0; i < size; ++i)
        nonzero += s->buffer[i] != 0;
    s->bytes += (uint64_t)size;
    if (s->frames == 0 &&
        !media_copy(s->first_info, sizeof(s->first_info), s->info, sizeof(s->info)))
        return false;
    ++s->frames;
    ++s->annexb_frames;
    s->nonzero_bytes += nonzero;
    if (!video_collect_parameters(&s->parameters, s->buffer, size, s->codec))
        ++s->parameter_errors;
    return true;
}

R3_MEDIA_LOCAL bool video_cleanup(const struct video_api *api, struct video_state *s)
{
    if (s->started) {
        int rc = api->stop(s->handle);
        printf("R3_VIDEO_CLEANUP operation=stop rc=0x%08x\n", (unsigned)rc);
        s->cleanup_failed |= rc != 0;
        s->started = false;
    }
    if (s->handle) {
        int rc = api->close(s->handle);
        printf("R3_VIDEO_CLEANUP operation=close rc=0x%08x\n", (unsigned)rc);
        s->cleanup_failed |= rc != 0;
        s->handle = NULL;
    }
    if (s->initialized) {
        int rc = api->terminate();
        printf("R3_VIDEO_CLEANUP operation=terminate rc=0x%08x\n", (unsigned)rc);
        s->cleanup_failed |= rc != 0;
        s->initialized = false;
    }
    return !s->cleanup_failed;
}

R3_MEDIA_LOCAL uint32_t video_timing_percentile99(const struct video_state *s, bool *valid)
{
    *valid = s->timing_count != 0 && s->timing_overflow == 0;
    if (!*valid)
        return 0;
    uint32_t sorted[R3_VIDEO_TIMING_CAPACITY];
    for (size_t i = 0; i < s->timing_count; ++i) {
        uint32_t value = s->timing_us[i];
        size_t j = i;
        while (j != 0 && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = value;
    }
    size_t rank = (99 * s->timing_count + 99) / 100;
    return sorted[rank - 1];
}
#endif

static void describe(void)
{
    puts("R3_MEDIA kind=source_admission hardware_evidence=absent");
    static const struct {
        unsigned width;
        unsigned height;
        unsigned fps;
        const char *range;
        const char *codec;
    } required[] = {{1280, 720, 30, "sdr", "h264"},
                    {1280, 720, 60, "sdr", "h264"},
                    {1920, 1080, 30, "sdr", "h264"},
                    {1920, 1080, 60, "sdr", "h264"},
                    {1920, 1080, 60, "hdr", "hevc_main10"}};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        printf("R3_MEDIA_REQUIRED width=%u height=%u fps=%u range=%s codec=%s "
               "status=unavailable reason=no_admitted_capture_encoder_abi "
               "frames_observed=absent duration_us=absent\n",
               required[i].width, required[i].height, required[i].fps, required[i].range,
               required[i].codec);
    }
    puts("R3_MEDIA_REQUIRED capability=stereo_game_audio status=unverified "
         "reason=public_avcap2_candidate_not_locally_validated");
    puts("R3_MEDIA_REQUIRED capability=hdr_to_sdr status=unavailable "
         "reason=no_admitted_bounded_conversion_abi");
    puts("R3_MEDIA_REQUIRED capability=source_hdr_metadata status=unavailable "
         "reason=no_independent_mastering_light_getter");
    puts("R3_MEDIA_REQUIRED capability=capture_protection status=unavailable "
         "reason=no_admitted_video_audio_epoch_signal");
    puts("R3_MEDIA_REQUIRED capability=resource_cleanup status=untested "
         "reason=no_media_resource_acquired");
}

#ifndef R3_HOST
/* Module lifecycle declarations: pinned SDK crt/rtld_sprx.c. Unlike its generic
 * loader, record and unload only modules this experiment actually loaded. */
int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t, void *, int *);
int sceKernelStopUnloadModule(int, size_t, const void *, uint32_t, void *, int *);

static int module_unload(void *context, int handle, int *result)
{
    (void)context;
    return sceKernelStopUnloadModule(handle, 0, NULL, 0, NULL, result);
}

static bool module_observe(void *context, const struct media_module *module)
{
    const char *probe = context;
    struct timespec now;
    bool clock_valid = clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec >= 0 &&
                       now.tv_nsec >= 0 && now.tv_nsec < 1000000000L &&
                       (uint64_t)now.tv_sec <= (UINT64_MAX - 999999) / 1000000;
    printf("R3_%s_CLEANUP%s operation=module_unload name=%s handle=0x%08x ", probe,
           module->completed ? "" : "_BEGIN", module->name, (unsigned)module->handle);
    if (clock_valid)
        printf("monotonic_us=%" PRIu64 " ",
               (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000);
    else
        printf("monotonic_us=absent ");
    if (module->completed)
        printf("rc=0x%08x result=0x%08x\n", (unsigned)module->rc, (unsigned)module->result);
    else
        puts("service_call_deadline=unknown");
    return fflush(stdout) != EOF && !ferror(stdout) && clock_valid;
}

static bool module_cleanup(const char *probe, struct media_modules *state, bool prerequisites_ok)
{
    const struct media_module_api api = {module_unload, module_observe, (void *)probe};
    if (!prerequisites_ok) {
        printf("R3_%s_CLEANUP operation=module_unload status=skipped "
               "reason=service_cleanup_failed_or_unverified\n",
               probe);
        (void)fflush(stdout);
    }
    return media_modules_cleanup(&api, state, prerequisites_ok);
}

static volatile sig_atomic_t audio_cancelled;
static void audio_cancel(int signum)
{
    audio_cancelled = signum;
}

static bool monotonic_us(uint64_t *out)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1000000000L || (uint64_t)now.tv_sec > (UINT64_MAX - 999999) / 1000000) {
        puts("R3_AUDIO_REFUSED reason=invalid_monotonic_clock");
        return false;
    }
    *out = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
    return true;
}

static int audio_probe(void)
{
    uint32_t firmware = kernel_get_fw_version();
    printf("R3_AUDIO_CONTEXT pid=%ld firmware_raw=0x%08" PRIx32 "\n", (long)getpid(), firmware);
    if ((firmware & UINT32_C(0xffff0000)) != UINT32_C(0x13600000))
        return 1;
    puts("R3_AUDIO_BEGIN planned_us=5000000 max_reads=5000 sample_storage_bytes=8192 "
         "metadata_bytes=16 service_call_deadline=unknown protection=unknown "
         "source_timestamps=unknown forwarding=none credential_fallback=none "
         "stock_sdk_startup=credential_and_syscall_writes");
    if (fflush(stdout) == EOF)
        return 1;
    struct sigaction action = {0}, old_int, old_term;
    action.sa_handler = audio_cancel;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, &old_int) != 0)
        return 1;
    if (sigaction(SIGTERM, &action, &old_term) != 0) {
        int restore = sigaction(SIGINT, &old_int, NULL);
        printf("R3_AUDIO_CLEANUP operation=restore_sigint rc=%d\n", restore);
        return 1;
    }
    audio_cancelled = 0;
    struct audio_state state = {0};
    struct audio_api api = {0};
    int modules[2] = {-1, -1};
    bool owned[2] = {false, false};
    struct media_modules cleanup_modules = {.count = 2};
    bool ok = true;
    uint64_t begin = 0, end = 0;
    bool interval_valid = false;
    const char *const paths[] = {"/system/common/lib/libSceIpmi.sprx",
                                 "/system/common/lib/libSceAvcap2.sprx"};
    const char *const names[] = {"libSceIpmi.sprx", "libSceAvcap2.sprx"};
    for (size_t i = 0; i < 2; ++i) {
        uint32_t existing = 0;
        int rc = kernel_dynlib_handle(getpid(), names[i], &existing);
        if (rc == 0 && existing != 0 && existing <= INT32_MAX) {
            modules[i] = (int)existing;
            printf("R3_AUDIO_MODULE name=%s ownership=preexisting\n", names[i]);
        } else {
            int result = 0;
            modules[i] = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &result);
            owned[i] = modules[i] >= 0;
            printf("R3_AUDIO_MODULE name=%s load_rc=0x%08x result=0x%08x\n", names[i],
                   (unsigned)modules[i], (unsigned)result);
            if (!owned[i] || result != 0) {
                ok = false;
                goto cleanup;
            }
        }
    }
    /* Binding retains exactly the public caller types in audio_api. */
#define AUDIO_RESOLVE(field, type, nid)                                                            \
    do {                                                                                           \
        uintptr_t address = kernel_dynlib_resolve(getpid(), (uint32_t)modules[1], nid);            \
        bool resolved = media_bind_pointer((void *)&api.field, sizeof(api.field), address);        \
        if (!resolved) {                                                                           \
            puts("R3_AUDIO_REFUSED reason=missing_export field=" #field);                          \
            ok = false;                                                                            \
            goto cleanup;                                                                          \
        }                                                                                          \
    } while (0)
    AUDIO_RESOLVE(initialize, int (*)(void), "svzPXluOz8U");
    AUDIO_RESOLVE(terminate, int (*)(void), "gojqghDU+1Y");
    AUDIO_RESOLVE(open, int (*)(void **, const void *, uint8_t), "3nHpE7Dp5SE");
    AUDIO_RESOLVE(start, int (*)(void *), "8CGNCwBsItI");
    AUDIO_RESOLVE(stop, int (*)(void *), "Z5dKK0xnQ8g");
    AUDIO_RESOLVE(close, int (*)(void *), "tR7gPe1i8hw");
    AUDIO_RESOLVE(read, int (*)(void *, void *, size_t, void *, uint32_t), "WhsHggqdwcg");
#undef AUDIO_RESOLVE
    if (audio_cancelled || !audio_start(&api, &state) || !monotonic_us(&begin)) {
        ok = false;
        goto cleanup;
    }
    end = begin;
    interval_valid = true;
    while (!audio_cancelled && state.reads < 5000 && end - begin < UINT64_C(5000000)) {
        if (!audio_read(&api, &state)) {
            ok = false;
            break;
        }
        struct timespec delay = {.tv_nsec = 1000000};
        if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
            ok = false;
            break;
        }
        uint64_t next;
        if (!monotonic_us(&next) || next < end) {
            interval_valid = false;
            ok = false;
            break;
        }
        end = next;
    }
cleanup:
    ok = audio_cleanup(&api, &state) && ok;
    printf("R3_AUDIO_COUNTS reads=%" PRIu64 " frames=%" PRIu64 " bytes=%" PRIu64 " empty=%" PRIu64
           " overruns=%" PRIu64 " left_nonzero=%" PRIu64 " right_nonzero=%" PRIu64
           " left_peak=%g right_peak=%g cancelled=%d\n",
           state.reads, state.frames, state.bytes, state.empty, state.overruns, state.nonzero[0],
           state.nonzero[1], (double)state.peak[0], (double)state.peak[1], (int)audio_cancelled);
    if (interval_valid)
        printf("R3_AUDIO_INTERVAL domain=console_monotonic elapsed_us=%" PRIu64 "\n", end - begin);
    else
        puts("R3_AUDIO_INTERVAL elapsed_us=absent reason=missing_or_invalid_clock");
    puts("R3_AUDIO_EVIDENCE sample_rate_hz=48000 rate_source=public_caller "
         "source_clock=unknown stereo_order=unverified protection=unknown "
         "driver_allocations=absent independent_cleanup=unverified");
    for (size_t i = 0; i < cleanup_modules.count; ++i)
        cleanup_modules.entries[i] =
            (struct media_module){.name = names[i], .handle = modules[i], .owned = owned[i]};
    if (!module_cleanup("AUDIO", &cleanup_modules, !state.cleanup_failed))
        ok = false;
    if (sigaction(SIGTERM, &old_term, NULL) != 0)
        ok = false;
    if (sigaction(SIGINT, &old_int, NULL) != 0)
        ok = false;
    puts("R3_AUDIO_RESULT status=incomplete readiness_gate=open");
    return fflush(stdout) == EOF || ferror(stdout) || !ok ? 1 : R3_INCOMPLETE;
}

static volatile sig_atomic_t video_cancelled;
static void video_cancel(int signum)
{
    video_cancelled = signum;
}

static bool resource_cleanup_wait(void *context)
{
    (void)context;
    struct timespec delay = {.tv_nsec = 16000000};
    return !video_cancelled && nanosleep(&delay, NULL) == 0 && !video_cancelled;
}

static bool video_monotonic_us(uint64_t *out)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1000000000L || (uint64_t)now.tv_sec > (UINT64_MAX - 999999) / 1000000) {
        puts("R3_VIDEO_REFUSED reason=invalid_monotonic_clock");
        return false;
    }
    *out = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
    return true;
}

static _Atomic uint32_t resource_notifications;
static _Atomic int32_t resource_last_operation;

static void resource_notified(int32_t resource_id, int32_t operation, const void *opaque)
{
    (void)opaque;
    if (resource_id == R3_VIDEO_RESOURCE_ID && (operation == 1 || operation == 2)) {
        atomic_store_explicit(&resource_last_operation, operation, memory_order_relaxed);
        atomic_fetch_add_explicit(&resource_notifications, 1, memory_order_relaxed);
    }
}

static void resource_print_selectors(const char *phase, const struct resource_state *s)
{
    printf("R3_RESOURCE_SELECTORS phase=%s available=%s values=", phase,
           s->available ? "yes" : "no");
    for (size_t i = 0; i < R3_RESOURCE_SELECTORS; ++i)
        printf("%s%08" PRIx32, i == 0 ? "" : ",", (uint32_t)s->selectors[i]);
    putchar('\n');
}

#ifndef R3_VIDEO_DURATION_US
#define R3_VIDEO_DURATION_US UINT64_C(10000000)
#endif

static int video_probe(void)
{
    uint32_t firmware = kernel_get_fw_version();
    printf("R3_VIDEO_CONTEXT pid=%ld firmware_raw=0x%08" PRIx32 "\n", (long)getpid(), firmware);
    if ((firmware & UINT32_C(0xffff0000)) != UINT32_C(0x13600000))
        return 1;
    printf("R3_VIDEO_BEGIN planned_us=%" PRIu64 " max_reads=20000 buffer_bytes=2097152 "
           "info_bytes=112 request_mode=0 source=2 selector=0xb4 "
           "resource_wait_us=5000000 resource_max_queries=320 cleanup_queries=64 "
           "read_flags=blocking service_call_deadline=unknown protection=unknown "
           "forwarding=none credential_fallback=none "
           "stock_sdk_startup=credential_and_syscall_writes\n",
           (uint64_t)R3_VIDEO_DURATION_US);
    if (fflush(stdout) == EOF)
        return 1;
    struct sigaction action = {0}, old_int, old_term;
    action.sa_handler = video_cancel;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, &old_int) != 0)
        return 1;
    if (sigaction(SIGTERM, &action, &old_term) != 0) {
        int restore = sigaction(SIGINT, &old_int, NULL);
        printf("R3_VIDEO_CLEANUP operation=restore_sigint rc=%d\n", restore);
        return 1;
    }
    video_cancelled = 0;
    static struct video_state state;
    state = (struct video_state){0};
#ifdef R3_MEDIA_SAVE_VIDEO
    static struct video_recording recording;
    unsigned char *recording_bytes = (unsigned char *)&recording;
    for (size_t i = 0; i < sizeof(recording); ++i)
        recording_bytes[i] = 0;
    state.recording = &recording;
    puts("R3_VIDEO_RETENTION enabled=1 capacity_bytes=2097152 capacity_records=1024 "
         "export_phase=after_cleanup");
#endif
#ifndef R3_VIDEO_TIER
#define R3_VIDEO_TIER 3
#endif
#ifndef R3_VIDEO_FPS
#define R3_VIDEO_FPS 30
#endif
#ifndef R3_VIDEO_BITRATE_KBPS
#define R3_VIDEO_BITRATE_KBPS 10000
#endif
#ifndef R3_VIDEO_CODEC
#define R3_VIDEO_CODEC 0
#endif
#ifndef R3_VIDEO_DEPTH
#define R3_VIDEO_DEPTH 8
#endif
#ifndef R3_VIDEO_CHECKPOINT
#define R3_VIDEO_CHECKPOINT R3_VIDEO_STREAM
#endif
    state.tier = R3_VIDEO_TIER;
    state.fps = R3_VIDEO_FPS;
    state.bitrate_kbps = R3_VIDEO_BITRATE_KBPS;
    state.codec = R3_VIDEO_CODEC;
    state.depth = R3_VIDEO_DEPTH;
    state.checkpoint = R3_VIDEO_CHECKPOINT;
    printf("R3_VIDEO_CONFIG tier=%u fps=%u bitrate_kbps=%" PRIu32 " width=%u height=%u "
           "codec=%s depth=%u request_peak_bytes_per_second=%" PRIu32 " checkpoint=%u\n",
           state.tier, state.fps, state.bitrate_kbps, state.tier == 3 ? 1280U : 1920U,
           state.tier == 3 ? 720U : 1080U, state.codec == 0 ? "avc" : "hevc", state.depth,
           state.bitrate_kbps * 1000U, (unsigned)state.checkpoint);
    struct video_api api = {0};
    struct resource_api resource = {0};
    struct resource_state resource_state = {.pid = (int32_t)getpid()};
    int modules[3] = {-1, -1, -1};
    bool owned[3] = {false, false, false};
    struct media_modules cleanup_modules = {.count = 3};
    bool ok = true;
    bool resource_ready = false;
    uint64_t begin = 0, end = 0;
    bool interval_valid = false;
    const char *const paths[] = {"/system/common/lib/libSceIpmi.sprx",
                                 "/system/common/lib/libSceResourceArbitrator.sprx",
                                 "/system/common/lib/libSceAvcap2.sprx"};
    const char *const names[] = {"libSceIpmi.sprx", "libSceResourceArbitrator.sprx",
                                 "libSceAvcap2.sprx"};
    atomic_store_explicit(&resource_notifications, 0, memory_order_relaxed);
    atomic_store_explicit(&resource_last_operation, -1, memory_order_relaxed);
    for (size_t i = 0; i < 3; ++i) {
        uint32_t existing = 0;
        int rc = kernel_dynlib_handle(getpid(), names[i], &existing);
        if (rc == 0 && existing != 0 && existing <= INT32_MAX) {
            modules[i] = (int)existing;
            printf("R3_VIDEO_MODULE name=%s ownership=preexisting\n", names[i]);
        } else {
            int result = 0;
            modules[i] = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &result);
            owned[i] = modules[i] >= 0;
            printf("R3_VIDEO_MODULE name=%s load_rc=0x%08x result=0x%08x\n", names[i],
                   (unsigned)modules[i], (unsigned)result);
            if (!owned[i] || result != 0) {
                ok = false;
                goto cleanup;
            }
        }
    }
#define VIDEO_RESOLVE(field, type, nid)                                                            \
    do {                                                                                           \
        uintptr_t address = kernel_dynlib_resolve(getpid(), (uint32_t)modules[2], nid);            \
        bool resolved = media_bind_pointer((void *)&api.field, sizeof(api.field), address);        \
        if (!resolved) {                                                                           \
            puts("R3_VIDEO_REFUSED reason=missing_export field=" #field);                          \
            ok = false;                                                                            \
            goto cleanup;                                                                          \
        }                                                                                          \
    } while (0)
    VIDEO_RESOLVE(initialize, int (*)(void), "svzPXluOz8U");
    VIDEO_RESOLVE(terminate, int (*)(void), "gojqghDU+1Y");
    VIDEO_RESOLVE(open, int (*)(void **, const void *, uint8_t), "4jwUgirLFrA");
    VIDEO_RESOLVE(start, int (*)(void *), "8CGNCwBsItI");
    VIDEO_RESOLVE(stop, int (*)(void *), "Z5dKK0xnQ8g");
    VIDEO_RESOLVE(close, int (*)(void *), "tR7gPe1i8hw");
    VIDEO_RESOLVE(read, int (*)(void *, void *, size_t, void *, uint32_t), "pN0hIQZsMzc");
    VIDEO_RESOLVE(query_mode_flags, int (*)(uint32_t *), "DHFDHdKAr9U");
    VIDEO_RESOLVE(query_capture_state, int (*)(void *), "RW88AiwmL+Y");
#ifdef R3_MEDIA_HEADERS
    VIDEO_RESOLVE(query_headers, int (*)(const void *, void *), "VAFgNc0Pfp8");
#endif
#undef VIDEO_RESOLVE

#define RESOURCE_RESOLVE(field, type, nid)                                                         \
    do {                                                                                           \
        uintptr_t address = kernel_dynlib_resolve(getpid(), (uint32_t)modules[1], nid);            \
        bool resolved =                                                                            \
            media_bind_pointer((void *)&resource.field, sizeof(resource.field), address);          \
        if (!resolved) {                                                                           \
            puts("R3_VIDEO_REFUSED reason=missing_resource_export field=" #field);                 \
            ok = false;                                                                            \
            goto cleanup;                                                                          \
        }                                                                                          \
    } while (0)
    RESOURCE_RESOLVE(register_client, int (*)(int32_t, resource_callback, void *), "kJvjeet+f9I");
    RESOURCE_RESOLVE(unregister_client, int (*)(int32_t), "LqmYZGB-UEQ");
    RESOURCE_RESOLVE(request_state, int (*)(int32_t, const char *, void *, int32_t), "CYjfezkWcaE");
    RESOURCE_RESOLVE(query_selectors, int (*)(int32_t *), "fBX+DBOwNis");
#undef RESOURCE_RESOLVE

    if (!resource_query(&resource, &resource_state)) {
        ok = false;
        goto cleanup;
    }
    resource_print_selectors("before", &resource_state);
    if (resource_state.available) {
        puts("R3_VIDEO_REFUSED reason=selector_already_available id=0xb4");
        goto cleanup;
    }
    uint64_t resource_begin = 0;
    if (video_cancelled || !video_monotonic_us(&resource_begin) ||
        !resource_start(&resource, &resource_state, resource_notified)) {
        ok = false;
        goto cleanup;
    }
    for (unsigned i = 0; i < 320 && !video_cancelled; ++i) {
        uint64_t now = 0;
        if (!resource_query(&resource, &resource_state) || !video_monotonic_us(&now) ||
            now < resource_begin) {
            ok = false;
            break;
        }
        if (resource_state.available) {
            resource_ready = true;
            break;
        }
        if (now - resource_begin >= UINT64_C(5000000))
            break;
        struct timespec delay = {.tv_nsec = 16000000};
        if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
            ok = false;
            break;
        }
    }
    resource_print_selectors("requested", &resource_state);
    if (!resource_ready) {
        puts("R3_VIDEO_REFUSED reason=resource_selector_not_available id=0xb4");
        ok = false;
        goto cleanup;
    }
    if (video_cancelled) {
        ok = false;
        goto cleanup;
    }
    bool started = video_start(&api, &state);
    if (state.checkpoint != R3_VIDEO_STREAM) {
        bool expected =
            state.checkpoint == R3_VIDEO_OPEN_ONLY
                ? started && state.handle && !state.started
                : !started && !state.handle && (uint32_t)state.open_result == UINT32_C(0x81950001);
        printf("R3_VIDEO_CHECKPOINT expected_result=%s open_rc=0x%08x started=%d\n",
               expected ? "observed" : "absent", (unsigned)state.open_result, (int)state.started);
        ok = expected;
        goto cleanup;
    }
    if (!started || !video_monotonic_us(&begin)) {
        ok = false;
        goto cleanup;
    }
    end = begin;
    interval_valid = true;
    while (!video_cancelled && state.reads < 20000 &&
           end - begin < (uint64_t)R3_VIDEO_DURATION_US) {
        uint64_t call_begin = end;
        uint64_t frames_before = state.frames;
        if (!video_read(&api, &state)) {
            ok = false;
            break;
        }
        uint64_t next;
        if (!video_monotonic_us(&next) || next < call_begin) {
            interval_valid = false;
            ok = false;
            break;
        }
        if (state.frames != frames_before) {
            uint64_t elapsed = next - call_begin;
            if (state.timing_count < R3_VIDEO_TIMING_CAPACITY)
                state.timing_us[state.timing_count++] =
                    elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
            else
                ++state.timing_overflow;
        }
        end = next;
        struct timespec delay = {.tv_nsec = 1000000};
        if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
            ok = false;
            break;
        }
        if (!video_monotonic_us(&end) || end < next) {
            interval_valid = false;
            ok = false;
            break;
        }
    }
cleanup:
    ok = video_cleanup(&api, &state) && ok;
    bool did_register = resource_state.registered;
    if (!resource_cleanup(&resource, &resource_state))
        ok = false;
    if (did_register && !resource_state.cleanup_failed) {
        bool selector_removed =
            resource_wait_removed(&resource, &resource_state, resource_cleanup_wait, NULL);
        resource_print_selectors("after_cleanup", &resource_state);
        if (!selector_removed) {
            puts("R3_VIDEO_CLEANUP selector_removal=unverified");
            ok = false;
        }
    }
    printf("R3_VIDEO_RESOURCE registered=%d requested=%d selector_available=%d "
           "queries=%" PRIu32 " notifications=%" PRIu32 " last_operation=%" PRId32 "\n",
           (int)resource_state.registered, (int)resource_state.requested,
           (int)resource_state.available, resource_state.queries,
           atomic_load_explicit(&resource_notifications, memory_order_relaxed),
           atomic_load_explicit(&resource_last_operation, memory_order_relaxed));
    bool timing_valid = false;
    uint32_t p99 = video_timing_percentile99(&state, &timing_valid);
    printf("R3_VIDEO_COUNTS reads=%" PRIu64 " frames=%" PRIu64 " bytes=%" PRIu64
           " annexb_frames=%" PRIu64 " nonzero_bytes=%" PRIu64 " empty=%" PRIu64
           " overruns=%" PRIu64 " cancelled=%d\n",
           state.reads, state.frames, state.bytes, state.annexb_frames, state.nonzero_bytes,
           state.empty, state.overruns, (int)video_cancelled);
    if (interval_valid) {
        uint64_t elapsed = end - begin;
        printf("R3_VIDEO_INTERVAL domain=console_monotonic elapsed_us=%" PRIu64
               " measured_bitrate_bps=%" PRIu64 "\n",
               elapsed, elapsed ? state.bytes * 8 * 1000000 / elapsed : 0);
    } else {
        puts("R3_VIDEO_INTERVAL elapsed_us=absent measured_bitrate_bps=absent "
             "reason=missing_or_invalid_clock");
    }
    if (timing_valid)
        printf("R3_VIDEO_TIMING population=%zu p99_read_us=%" PRIu32 " overflow=%" PRIu64 "\n",
               state.timing_count, p99, state.timing_overflow);
    else
        printf("R3_VIDEO_TIMING population=%zu p99_read_us=absent overflow=%" PRIu64 "\n",
               state.timing_count, state.timing_overflow);
    printf("R3_VIDEO_PARAMETERS count=%zu bytes=%zu overflow=%d errors=%" PRIu64 " changes=%" PRIu64
           " picture_payload_saved=%s\n",
           state.parameters.count, state.parameters.used, (int)state.parameters.overflow,
           state.parameter_errors, state.parameters.changes,
           state.recording ? "pending_export" : "no");
    for (size_t i = 0; i < state.parameters.count; ++i) {
        printf(
            "R3_VIDEO_PARAMETER codec=%s type=%u bytes=%zu hex=", state.codec == 0 ? "avc" : "hevc",
            state.parameters.type[i], state.parameters.length[i]);
        for (size_t j = 0; j < state.parameters.length[i]; ++j)
            printf("%02x", state.parameters.bytes[state.parameters.offset[i] + j]);
        putchar('\n');
    }
    if (state.frames != 0) {
        printf("R3_VIDEO_FIRST_INFO interpretation=opaque hex=");
        for (size_t i = 0; i < sizeof(state.first_info); ++i)
            printf("%02x", state.first_info[i]);
        putchar('\n');
    }
    puts("R3_VIDEO_EVIDENCE encoder_path=avcap2_to_sceavcapture_venc "
         "codec_profile=unverified dynamic_range=unverified source_metadata=unverified "
         "protection=unknown driver_allocations=absent independent_cleanup=unverified");
    for (size_t i = 0; i < cleanup_modules.count; ++i)
        cleanup_modules.entries[i] =
            (struct media_module){.name = names[i], .handle = modules[i], .owned = owned[i]};
    if (!module_cleanup("VIDEO", &cleanup_modules,
                        !state.cleanup_failed && !resource_state.cleanup_failed))
        ok = false;
    if (sigaction(SIGTERM, &old_term, NULL) != 0)
        ok = false;
    if (sigaction(SIGINT, &old_int, NULL) != 0)
        ok = false;
    if (!video_export(stdout, state.recording,
                      ok && !video_cancelled && interval_valid &&
                          end - begin >= (uint64_t)R3_VIDEO_DURATION_US &&
                          !state.parameter_errors && !state.parameters.overflow &&
                          !state.parameters.changes && state.parameters.count != 0 &&
                          state.recording && state.recording->count == state.frames &&
                          state.recording->used == state.bytes))
        ok = false;
    puts("R3_VIDEO_RESULT status=incomplete readiness_gate=open");
    return fflush(stdout) == EOF || ferror(stdout) || !ok ? 1 : R3_INCOMPLETE;
}

static int resource_probe(void)
{
    uint32_t firmware = kernel_get_fw_version();
    printf("R3_RESOURCE_CONTEXT pid=%ld firmware_raw=0x%08" PRIx32 "\n", (long)getpid(), firmware);
    if ((firmware & UINT32_C(0xffff0000)) != UINT32_C(0x13600000))
        return 1;
    puts("R3_RESOURCE_BEGIN id=0xb4 planned_us=5000000 max_queries=320 "
         "cleanup_queries=64 existing_available_id=refuse media_calls=none "
         "service_call_deadline=unknown stock_sdk_startup=credential_and_syscall_writes");
    if (fflush(stdout) == EOF)
        return 1;
    struct sigaction action = {0}, old_int, old_term;
    action.sa_handler = video_cancel;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, &old_int) != 0)
        return 1;
    if (sigaction(SIGTERM, &action, &old_term) != 0) {
        int restore = sigaction(SIGINT, &old_int, NULL);
        printf("R3_RESOURCE_CLEANUP operation=restore_sigint rc=%d\n", restore);
        return 1;
    }
    video_cancelled = 0;
    atomic_store_explicit(&resource_notifications, 0, memory_order_relaxed);
    atomic_store_explicit(&resource_last_operation, -1, memory_order_relaxed);
    struct resource_state state = {.pid = (int32_t)getpid()};
    struct resource_api api = {0};
    int modules[2] = {-1, -1};
    bool owned[2] = {false, false};
    struct media_modules cleanup_modules = {.count = 2};
    bool ok = true, did_register = false, observed = false;
    uint64_t begin = 0, now = 0;
    const char *const names[] = {"libSceIpmi.sprx", "libSceResourceArbitrator.sprx"};
    const char *const paths[] = {"/system/common/lib/libSceIpmi.sprx",
                                 "/system/common/lib/libSceResourceArbitrator.sprx"};
    for (size_t i = 0; i < 2; ++i) {
        uint32_t existing = 0;
        int rc = kernel_dynlib_handle(getpid(), names[i], &existing);
        if (rc == 0 && existing != 0 && existing <= INT32_MAX) {
            modules[i] = (int)existing;
            printf("R3_RESOURCE_MODULE name=%s ownership=preexisting\n", names[i]);
        } else {
            int result = 0;
            modules[i] = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &result);
            owned[i] = modules[i] >= 0;
            printf("R3_RESOURCE_MODULE name=%s load_rc=0x%08x result=0x%08x\n", names[i],
                   (unsigned)modules[i], (unsigned)result);
            if (!owned[i] || result != 0) {
                ok = false;
                goto cleanup;
            }
        }
    }
#define RESOURCE_RESOLVE(field, type, nid)                                                         \
    do {                                                                                           \
        uintptr_t address = kernel_dynlib_resolve(getpid(), (uint32_t)modules[1], nid);            \
        bool resolved = media_bind_pointer((void *)&api.field, sizeof(api.field), address);        \
        if (!resolved) {                                                                           \
            puts("R3_RESOURCE_REFUSED reason=missing_export field=" #field);                       \
            ok = false;                                                                            \
            goto cleanup;                                                                          \
        }                                                                                          \
    } while (0)
    RESOURCE_RESOLVE(register_client, int (*)(int32_t, resource_callback, void *), "kJvjeet+f9I");
    RESOURCE_RESOLVE(unregister_client, int (*)(int32_t), "LqmYZGB-UEQ");
    RESOURCE_RESOLVE(request_state, int (*)(int32_t, const char *, void *, int32_t), "CYjfezkWcaE");
    RESOURCE_RESOLVE(query_selectors, int (*)(int32_t *), "fBX+DBOwNis");
#undef RESOURCE_RESOLVE
    if (!resource_query(&api, &state)) {
        ok = false;
        goto cleanup;
    }
    resource_print_selectors("before", &state);
    if (state.available) {
        puts("R3_RESOURCE_REFUSED reason=id_already_available");
        goto cleanup;
    }
    if (!video_monotonic_us(&begin) || !resource_start(&api, &state, resource_notified)) {
        ok = false;
        goto cleanup;
    }
    for (unsigned i = 0; i < 320 && !video_cancelled; ++i) {
        if (!resource_query(&api, &state) || !video_monotonic_us(&now) || now < begin) {
            ok = false;
            break;
        }
        if (state.available) {
            observed = true;
            break;
        }
        if (now - begin >= UINT64_C(5000000))
            break;
        struct timespec delay = {.tv_nsec = 16000000};
        if (nanosleep(&delay, NULL) != 0)
            break;
    }
    resource_print_selectors("requested", &state);
cleanup:
    did_register = state.registered;
    if (!resource_cleanup(&api, &state))
        ok = false;
    if (did_register && !state.cleanup_failed) {
        bool absent = resource_wait_removed(&api, &state, resource_cleanup_wait, NULL);
        resource_print_selectors("after_cleanup", &state);
        if (!absent) {
            puts("R3_RESOURCE_CLEANUP selector_removal=unverified");
            ok = false;
        }
    }
    printf("R3_RESOURCE_OBSERVATION selector_became_available=%s queries=%" PRIu32
           " notifications=%" PRIu32 " last_operation=%" PRId32 " stop_requested=%d\n",
           observed ? "yes" : "no", state.queries,
           atomic_load_explicit(&resource_notifications, memory_order_relaxed),
           atomic_load_explicit(&resource_last_operation, memory_order_relaxed),
           (int)video_cancelled);
    for (size_t i = 0; i < cleanup_modules.count; ++i)
        cleanup_modules.entries[i] =
            (struct media_module){.name = names[i], .handle = modules[i], .owned = owned[i]};
    if (!module_cleanup("RESOURCE", &cleanup_modules, !state.cleanup_failed))
        ok = false;
    if (sigaction(SIGTERM, &old_term, NULL) != 0)
        ok = false;
    if (sigaction(SIGINT, &old_int, NULL) != 0)
        ok = false;
    puts("R3_RESOURCE_RESULT status=incomplete readiness_gate=open");
    return fflush(stdout) == EOF || ferror(stdout) || !ok ? 1 : R3_INCOMPLETE;
}

static int inventory(void)
{
    /* SDK crt/kernel.c reads libSceLibcInternal's process parameter; this is
     * firmware context only, not a live jailbreak/hypervisor attestation. */
    uint32_t firmware = kernel_get_fw_version();
    printf("R3_MEDIA_CONTEXT pid=%ld firmware_raw=0x%08" PRIx32 "\n", (long)getpid(), firmware);
    if ((firmware & UINT32_C(0xffff0000)) != UINT32_C(0x13600000)) {
        puts("R3_MEDIA_REFUSED reason=unsupported_or_unknown_firmware");
        return 1;
    }
    static const char *const paths[] = {
        "/system/common/lib/libSceRemoteplay.sprx",     "/system/common/lib/libSceAvcap2.sprx",
        "/system/common/lib/libSceVideoRecording.sprx", "/system/common/lib/libSceVenc.sprx",
        "/system/common/lib/libSceAudioSystem.sprx",    "/system/common/lib/libSceAudioOut.sprx",
        "/system/common/lib/libSceVideoOut.sprx"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        struct stat info;
        errno = 0;
        int rc = stat(paths[i], &info);
        int saved_errno = rc == 0 ? 0 : errno;
        printf("R3_MEDIA_PATH path=%s stat_rc=%d errno=%d regular=%s "
               "callable=unknown permission=unknown\n",
               paths[i], rc, saved_errno,
               rc != 0 ? "unknown" : (S_ISREG(info.st_mode) ? "yes" : "no"));
    }
    puts("R3_MEDIA_INVENTORY media_resources_acquired=none probe_defined_privileged_changes=none "
         "stock_sdk_crt=per_process_credential_writes_before_main sdk_reversal=unverified "
         "status=incomplete reason=path_presence_is_not_a_callable_interface");
    return R3_INCOMPLETE;
}
#endif

#ifdef R3_MEDIA_TEST
int r3_media_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    bool request_audio = argc == 2 && strcmp(argv[1], "--audio-five-seconds") == 0;
#if defined(R3_MEDIA_AUDIO) && !defined(R3_HOST)
    request_audio = request_audio || argc <= 1;
#endif
    bool request_video = argc == 2 && strcmp(argv[1], "--video-ten-seconds") == 0;
#if defined(R3_MEDIA_VIDEO) && !defined(R3_HOST)
    request_video = request_video || argc <= 1;
#endif
    bool request_resource = argc == 2 && strcmp(argv[1], "--video-admission-five-seconds") == 0;
#if defined(R3_MEDIA_ADMISSION) && !defined(R3_HOST)
    request_resource = request_resource || argc <= 1;
#endif
    bool request_inventory = argc == 2 && strcmp(argv[1], "--inventory") == 0;
#if defined(R3_MEDIA_INVENTORY) && !defined(R3_HOST)
    /* Raw ELF-loader execution has no CLI argument transport. This separately
     * named build selects only the read-only inventory admission mode. */
    request_inventory = request_inventory || argc <= 1;
#endif
    if (argc > 2 || (argc == 2 && !request_inventory && !request_audio && !request_video &&
                     !request_resource && strcmp(argv[1], "--describe") != 0)) {
        return fputs("usage: capture-encode [--describe|--inventory|--audio-five-seconds|"
                     "--video-ten-seconds|--video-admission-five-seconds]\n",
                     stderr) == EOF
                   ? 1
                   : R3_USAGE;
    }
    if (request_resource) {
#ifdef R3_HOST
        return fputs("R3_RESOURCE_REFUSED reason=host_build_cannot_supply_console_evidence\n",
                     stderr) == EOF
                   ? 1
                   : R3_USAGE;
#else
        return resource_probe();
#endif
    }
    if (request_audio) {
#ifdef R3_HOST
        return fputs("R3_AUDIO_REFUSED reason=host_build_cannot_supply_console_evidence\n",
                     stderr) == EOF
                   ? 1
                   : R3_USAGE;
#else
        return audio_probe();
#endif
    }
    if (request_video) {
#ifdef R3_HOST
        return fputs("R3_VIDEO_REFUSED reason=host_build_cannot_supply_console_evidence\n",
                     stderr) == EOF
                   ? 1
                   : R3_USAGE;
#else
        return video_probe();
#endif
    }
    if (request_inventory) {
#ifdef R3_HOST
        return fputs("R3_MEDIA_REFUSED reason=host_build_cannot_supply_console_evidence\n",
                     stderr) == EOF
                   ? 1
                   : R3_USAGE;
#else
        int rc = inventory();
        if (rc != R3_INCOMPLETE) {
            return rc;
        }
#endif
    }
    describe();
    puts("R3_MEDIA_RESULT status=incomplete readiness_gate=open");
    return fflush(stdout) == EOF || ferror(stdout) ? 1 : R3_INCOMPLETE;
}
