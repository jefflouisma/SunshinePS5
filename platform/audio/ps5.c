#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "ps5.h"

#include "firmware/13.60.h"
#include "ps5_media_global.h"
#include "runtime.h"

#include <errno.h>
#include <float.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Exact SDK declaration used by the retained probe:
 * a7685ce8 target/include/ps5/kernel.h. Re-declared so host syntax checks need
 * no PS5 sysroot; the shared media owner already loaded/resolved the module. */
intptr_t kernel_dynlib_resolve(pid_t pid, uint32_t handle, const char *nid);

/* Exact five audio-call NIDs from tools/ps5-probes/media_probe.h and
 * capture_encode.c; module lifetime and Initialize/Terminate are owned by the
 * shared ps5_media_global module. */
#define AVCAP2_OPEN_AUDIO_NID "3nHpE7Dp5SE"
#define AVCAP2_START_NID "8CGNCwBsItI"
#define AVCAP2_STOP_NID "Z5dKK0xnQ8g"
#define AVCAP2_CLOSE_NID "tR7gPe1i8hw"
#define AVCAP2_READ_AUDIO_NID "WhsHggqdwcg"

#define AVCAP2_EMPTY_1 UINT32_C(0x81950002)
#define AVCAP2_OVERRUN UINT32_C(0x81950004)
#define AVCAP2_ENDED UINT32_C(0x81950008)
#define AVCAP2_EMPTY_2 UINT32_C(0x8195000d)

/* Exact 56-byte request from media_probe.h:37-46. */
struct sp5_ps5_audio_request {
    uint64_t size, unknown08;
    uint32_t unknown10, source, mode, unknown1c;
    uint64_t unknown20;
    uint32_t bitrate, reserved2c;
    uint64_t unknown30;
};

_Static_assert(sizeof(struct sp5_ps5_audio_request) == 0x38, "Avcap2 request size");
_Static_assert(offsetof(struct sp5_ps5_audio_request, source) == 0x14, "Avcap2 source offset");
_Static_assert(offsetof(struct sp5_ps5_audio_request, unknown30) == 0x30, "Avcap2 request tail");
_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "Avcap2 invokes stereo binary32 samples");
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "Avcap2 x86-64 native sample bytes");
_Static_assert(sizeof(intptr_t) == sizeof(void *), "sourced flat function-pointer representation");

enum { READ_DATA = 0, READ_EMPTY, READ_OVERRUN, READ_ERROR };

/* One audio context per process, as required by the provisional contract.
 * Callers serialize access; this is a guard, not a concurrency primitive. */
static sp5_ps5_audio *active_context;

static sp5_result failure(sp5_reason reason)
{
    return sp5_failure(reason);
}

static bool bind_pointer(void *destination, size_t capacity, intptr_t address)
{
    if (address == 0 || capacity != sizeof(address) || capacity != sizeof(void *))
        return false;
    memcpy(destination, &address, sizeof(address));
    return true;
}

static sp5_result console_now(sp5_timestamp *out)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1000000000L ||
        (uint64_t)now.tv_sec > (UINT64_MAX - 999999U) / SP5_PS5_AUDIO_CLOCK_TICKS_PER_SECOND)
        return failure(SP5_REASON_CLOCK);
    out->ticks =
        (uint64_t)now.tv_sec * SP5_PS5_AUDIO_CLOCK_TICKS_PER_SECOND + (uint64_t)now.tv_nsec / 1000U;
    out->domain = SP5_PS5_AUDIO_CLOCK_DOMAIN;
    out->epoch = SP5_PS5_AUDIO_CLOCK_EPOCH;
    out->validity = SP5_VALIDITY_VALID;
    out->reason = SP5_REASON_NONE;
    return sp5_success();
}

static bool deadline_valid(sp5_timestamp deadline)
{
    return deadline.validity == SP5_VALIDITY_VALID &&
           deadline.domain == SP5_PS5_AUDIO_CLOCK_DOMAIN &&
           deadline.epoch == SP5_PS5_AUDIO_CLOCK_EPOCH;
}

static sp5_result resolve_api(sp5_ps5_audio *audio)
{
    const sp5_ps5_media_global_info *global = sp5_ps5_media_global_status();
    if (global == NULL || global->handle[SP5_PS5_MEDIA_GLOBAL_AVCAP2] < 0)
        return failure(SP5_REASON_UNSUPPORTED);
    uint32_t handle = (uint32_t)global->handle[SP5_PS5_MEDIA_GLOBAL_AVCAP2];
#define RESOLVE(field, nid)                                                                        \
    do {                                                                                           \
        intptr_t address = kernel_dynlib_resolve(getpid(), handle, nid);                           \
        if (!bind_pointer((void *)&audio->api.field, sizeof(audio->api.field), address))           \
            return failure(SP5_REASON_UNSUPPORTED);                                                \
    } while (0)
    RESOLVE(open, AVCAP2_OPEN_AUDIO_NID);
    RESOLVE(start, AVCAP2_START_NID);
    RESOLVE(stop, AVCAP2_STOP_NID);
    RESOLVE(close, AVCAP2_CLOSE_NID);
    RESOLVE(read, AVCAP2_READ_AUDIO_NID);
#undef RESOLVE
    return sp5_success();
}

static bool has_resources(const sp5_ps5_audio *audio)
{
    return audio->global_acquired || audio->global_release_pending || audio->started ||
           audio->handle != NULL;
}

static void remember_failure(sp5_ps5_audio *audio, sp5_reason *first, sp5_reason reason)
{
    if (reason == SP5_REASON_NONE)
        reason = SP5_REASON_PLATFORM;
    if (*first == SP5_REASON_NONE)
        *first = reason;
    if (audio->cleanup_error == SP5_REASON_NONE)
        audio->cleanup_error = reason;
}

/* H2: retain accurate handle/global ownership. A failed Stop still allows the
 * Close attempt; a successful Close clears the handle even if Stop reported an
 * error. A failed Close keeps the handle for a later retry. The global reference
 * is returned only after local teardown. A failed Terminate keeps the reference
 * pending; a Terminate that succeeded but left shared module unloading pending is
 * not an audio-owned wedge. */
static sp5_result close_state(sp5_ps5_audio *audio)
{
    audio->open = false;
    sp5_reason first = SP5_REASON_NONE;

    if (audio->started) {
        int rc = audio->api.stop != NULL ? audio->api.stop(audio->handle) : -1;
        if (rc == 0)
            audio->started = false;
        else
            remember_failure(audio, &first, SP5_REASON_PLATFORM);
    }
    if (audio->handle != NULL) {
        int rc = audio->api.close != NULL ? audio->api.close(audio->handle) : -1;
        if (rc == 0) {
            audio->handle = NULL;
            audio->started = false;
        } else {
            remember_failure(audio, &first, SP5_REASON_PLATFORM);
        }
    }
    if (audio->global_acquired && !audio->started && audio->handle == NULL) {
        sp5_result released = sp5_ps5_media_global_release();
        /* The reference is consumed by release even when cleanup fails. */
        audio->global_acquired = false;
        if (released.status != SP5_OK) {
            remember_failure(audio, &first, released.reason);
            const sp5_ps5_media_global_info *global = sp5_ps5_media_global_status();
            /* Retry only a Terminate failure with no intervening media owner. A
             * successful Terminate leaves only shared module unloading, which is
             * not an audio-owned resource. */
            if (global != NULL && sp5_ps5_media_global_refcount() == 0)
                audio->global_release_pending = true;
        }
    } else if (audio->global_release_pending) {
        if (sp5_ps5_media_global_refcount() == 0) {
            sp5_result released = sp5_ps5_media_global_release();
            if (released.status == SP5_OK) {
                audio->global_release_pending = false;
            } else {
                remember_failure(audio, &first, released.reason);
                const sp5_ps5_media_global_info *global = sp5_ps5_media_global_status();
                audio->global_release_pending =
                    global != NULL && (global->initialized || sp5_ps5_media_global_refcount() == 0);
            }
        } else {
            /* Another media client now owns the global lifetime; our reference
             * was already consumed. */
            audio->global_release_pending = false;
        }
    }
    if (has_resources(audio)) {
        if (first == SP5_REASON_NONE)
            first = SP5_REASON_PLATFORM;
        return failure(first);
    }
    if (first != SP5_REASON_NONE && !audio->cleanup_reported) {
        audio->cleanup_reported = true;
        return failure(first);
    }
    audio->cleanup_reported = false;
    if (active_context == audio)
        active_context = NULL;
    return sp5_success();
}

static sp5_result audio_open(void *context, uint32_t sample_rate)
{
    sp5_ps5_audio *audio = context;
    if (audio == NULL)
        return failure(SP5_REASON_INVALID_ARGUMENT);
    if (!sp5_ps5_firmware_supported())
        return failure(SP5_REASON_UNSUPPORTED);
    if (sample_rate != (uint32_t)SP5_PS5_AUDIO_SAMPLE_RATE)
        return failure(SP5_REASON_UNSUPPORTED);
    if (audio->open || has_resources(audio) || active_context != NULL)
        return failure(SP5_REASON_OWNERSHIP);

    active_context = audio;
    audio->cleanup_error = SP5_REASON_NONE;
    audio->handle = NULL;
    audio->global_acquired = false;
    audio->global_release_pending = false;
    audio->started = false;
    audio->sample_rate = 0;
    audio->source_observed = false;
    audio->cleanup_reported = false;
    audio->protection = SP5_PROTECTION_UNKNOWN;
    audio->source_epoch = 0;
    memset(&audio->api, 0, sizeof(audio->api));

    sp5_result result = sp5_ps5_media_global_acquire();
    if (result.status != SP5_OK) {
        active_context = NULL;
        return result;
    }
    audio->global_acquired = true;
    result = resolve_api(audio);
    if (result.status != SP5_OK)
        goto unwind;

    const struct sp5_ps5_audio_request request = {
        .size = 0x38,
        .unknown08 = 0x18,
        .unknown10 = 0,
        .source = 2,
        .mode = 0,
        .unknown1c = UINT32_MAX,
        .unknown20 = 0,
        .bitrate = 0,
        .reserved2c = 0,
        .unknown30 = 0,
    };
    void *handle = NULL;
    int rc = audio->api.open(&handle, &request, 0);
    if (rc != 0 || handle == NULL) {
        /* Probe rule: a failed Open does not establish ownership of a returned
         * non-null value, so it is never passed to Close. */
        result = failure(SP5_REASON_PLATFORM);
        goto unwind;
    }
    audio->handle = handle;
    rc = audio->api.start(handle);
    if (rc != 0) {
        result = failure(SP5_REASON_PLATFORM);
        goto unwind;
    }
    audio->started = true;
    audio->open = true;
    audio->sample_rate = sample_rate;
    return sp5_success();

unwind: {
    sp5_result cleanup = close_state(audio);
    if (cleanup.status != SP5_OK)
        return cleanup;
}
    return result;
}

static int read_once(sp5_ps5_audio *audio, size_t *bytes)
{
    if (audio->api.read == NULL)
        return READ_ERROR;
    memset(audio->metadata, 0, sizeof(audio->metadata));
    audio->metadata[0] = 0x10; /* HearBridge initialization retained as opaque bytes. */
    int rc =
        audio->api.read(audio->handle, audio->samples, sizeof(audio->samples), audio->metadata, 1);
    ++audio->reads;
    if (rc == 0 || (uint32_t)rc == AVCAP2_EMPTY_1 || (uint32_t)rc == AVCAP2_EMPTY_2) {
        ++audio->empties;
        return READ_EMPTY;
    }
    if ((uint32_t)rc == AVCAP2_OVERRUN) {
        ++audio->overruns;
        return READ_OVERRUN;
    }
    if ((uint32_t)rc == AVCAP2_ENDED || rc < 0 || (size_t)rc > sizeof(audio->samples) ||
        rc % 8 != 0)
        return READ_ERROR;
    size_t count = (size_t)rc / sizeof(float);
    for (size_t index = 0; index < count; ++index) {
        /* Avoid the target sysroot's qualified _Generic math macros. NaN fails
         * both comparisons; infinities fail the range. */
        if (!(audio->samples[index] >= -FLT_MAX && audio->samples[index] <= FLT_MAX))
            return READ_ERROR;
    }
    *bytes = (size_t)rc;
    return READ_DATA;
}

static sp5_result audio_acquire(void *context, sp5_timestamp deadline, sp5_audio_block *block)
{
    sp5_ps5_audio *audio = context;
    if (audio == NULL || block == NULL)
        return failure(SP5_REASON_INVALID_ARGUMENT);
    *block = (sp5_audio_block){0};
    if (!audio->open)
        return failure(SP5_REASON_OWNERSHIP);
    if (audio->leased)
        return failure(SP5_REASON_WOULD_BLOCK);
    if (!audio->source_observed)
        return failure(SP5_REASON_PROTECTION_UNKNOWN);
    if (audio->protection != SP5_PROTECTION_CLEAR)
        return failure(audio->protection == SP5_PROTECTION_ACTIVE ? SP5_REASON_PROTECTED
                                                                  : SP5_REASON_PROTECTION_UNKNOWN);
    if (!deadline_valid(deadline) || audio->next_lease == 0 || audio->next_lease == UINT64_MAX)
        return failure(deadline_valid(deadline) ? SP5_REASON_OVERFLOW : SP5_REASON_CLOCK);

    sp5_timestamp now = {0};
    sp5_result result = console_now(&now);
    if (result.status != SP5_OK)
        return result;
    if (now.ticks >= deadline.ticks)
        return failure(SP5_REASON_TIMEOUT);
    for (unsigned attempt = 0; attempt < SP5_PS5_AUDIO_MAX_READS_PER_ACQUIRE; ++attempt) {
        size_t bytes = 0;
        int state = read_once(audio, &bytes);
        if (state == READ_ERROR)
            return failure(SP5_REASON_PLATFORM);
        if (state == READ_DATA) {
            uint64_t lease = audio->next_lease++;
            audio->lease_id = lease;
            audio->leased = true;
            *block = (sp5_audio_block){
                .lease_id = lease,
                .source_epoch = audio->source_epoch,
                .timestamp = {.ticks = 0,
                              .domain = SP5_PS5_AUDIO_CLOCK_DOMAIN,
                              .epoch = SP5_PS5_AUDIO_CLOCK_EPOCH,
                              .validity = SP5_VALIDITY_UNAVAILABLE,
                              .reason = SP5_REASON_STAGE_UNAVAILABLE},
                .samples = {(const uint8_t *)audio->samples, bytes},
                .sample_rate = (uint32_t)SP5_PS5_AUDIO_SAMPLE_RATE,
                .frames = (uint32_t)(bytes / 8U),
                .channels = (uint8_t)SP5_PS5_AUDIO_CHANNELS,
                .sample_format_ref = SP5_PS5_AUDIO_SAMPLE_FORMAT_REF,
            };
            return sp5_success();
        }
        result = console_now(&now);
        if (result.status != SP5_OK)
            return result;
        if (now.ticks >= deadline.ticks)
            return failure(SP5_REASON_TIMEOUT);
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
        if (nanosleep(&delay, NULL) != 0 && errno != EINTR)
            return failure(SP5_REASON_PLATFORM);
    }
    return failure(SP5_REASON_TIMEOUT);
}

static sp5_result audio_release(void *context, uint64_t lease_id)
{
    sp5_ps5_audio *audio = context;
    if (audio == NULL)
        return failure(SP5_REASON_INVALID_ARGUMENT);
    if (!audio->leased || audio->lease_id != lease_id)
        return failure(SP5_REASON_OWNERSHIP);
    audio->leased = false;
    return sp5_success();
}

static sp5_result audio_close(void *context)
{
    sp5_ps5_audio *audio = context;
    if (audio == NULL)
        return failure(SP5_REASON_INVALID_ARGUMENT);
    if (audio->leased)
        return failure(SP5_REASON_OWNERSHIP);
    return close_state(audio);
}

void sp5_ps5_audio_init(sp5_ps5_audio *audio)
{
    if (audio == NULL)
        return;
    memset(audio, 0, sizeof(*audio));
    audio->protection = SP5_PROTECTION_UNKNOWN;
    audio->cleanup_error = SP5_REASON_NONE;
    audio->next_lease = 1;
}

sp5_audio_ops sp5_ps5_audio_ops(sp5_ps5_audio *audio)
{
    if (audio == NULL)
        return (sp5_audio_ops){0};
    return (sp5_audio_ops){
        .context = audio,
        .open = audio_open,
        .acquire = audio_acquire,
        .release = audio_release,
        .close = audio_close,
    };
}

sp5_result sp5_ps5_audio_observe_source(sp5_ps5_audio *audio, const sp5_source_state *state)
{
    if (audio == NULL || state == NULL)
        return failure(SP5_REASON_INVALID_ARGUMENT);
    if (state->observed_at.validity != SP5_VALIDITY_VALID)
        return failure(state->observed_at.reason == SP5_REASON_NONE ? SP5_REASON_UNVERIFIED
                                                                    : state->observed_at.reason);
    if (state->protection != SP5_PROTECTION_UNKNOWN && state->protection != SP5_PROTECTION_CLEAR &&
        state->protection != SP5_PROTECTION_ACTIVE)
        return failure(SP5_REASON_INVALID_ARGUMENT);
    audio->source_epoch = state->epoch;
    audio->protection = state->protection;
    audio->source_observed = true;
    return sp5_success();
}

sp5_result sp5_ps5_audio_now(sp5_timestamp *timestamp)
{
    if (timestamp == NULL)
        return failure(SP5_REASON_INVALID_ARGUMENT);
    *timestamp = (sp5_timestamp){0};
    return console_now(timestamp);
}
