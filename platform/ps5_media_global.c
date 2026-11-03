#include "ps5_media_global.h"

#include "firmware/13.60.h"
#include "runtime.h"

#include <limits.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__PROSPERO__)
#include <ps5/kernel.h>
int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t, void *, int *);
int sceKernelStopUnloadModule(int, size_t, const void *, uint32_t, void *, int *);
#endif

// NOLINTBEGIN(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)

typedef int (*sp5_global_lifecycle_fn)(void);

typedef struct {
    uint32_t refcount;
    int handle[SP5_PS5_MEDIA_GLOBAL_MODULE_COUNT];
    bool loaded[SP5_PS5_MEDIA_GLOBAL_MODULE_COUNT];
    bool owned[SP5_PS5_MEDIA_GLOBAL_MODULE_COUNT];
    bool attempted[SP5_PS5_MEDIA_GLOBAL_MODULE_COUNT];
    bool completed[SP5_PS5_MEDIA_GLOBAL_MODULE_COUNT];
    bool initialized;
    bool open;
    bool gate_ok;
    sp5_reason failure;
    sp5_global_lifecycle_fn initialize;
    sp5_global_lifecycle_fn terminate;
} sp5_global_state;

static sp5_global_state media = {
    .handle = {-1, -1, -1},
    .gate_ok = true,
};
static sp5_ps5_media_global_info published = {
    .handle = {-1, -1, -1},
};

#if defined(__PROSPERO__)
static const char *const module_paths[SP5_PS5_MEDIA_GLOBAL_MODULE_COUNT] = {
    "/system/common/lib/libSceIpmi.sprx",
    "/system/common/lib/libSceResourceArbitrator.sprx",
    "/system/common/lib/libSceAvcap2.sprx",
};
static const char *const module_names[SP5_PS5_MEDIA_GLOBAL_MODULE_COUNT] = {
    "libSceIpmi.sprx",
    "libSceResourceArbitrator.sprx",
    "libSceAvcap2.sprx",
};
static const char *const lifecycle_nids[2] = {"svzPXluOz8U", "gojqghDU+1Y"};
#endif

static void publish(void)
{
    published.refcount = media.refcount;
    published.initialized = media.initialized;
    published.failure = media.failure;
    for (size_t i = 0; i < SP5_PS5_MEDIA_GLOBAL_MODULE_COUNT; ++i) {
        published.handle[i] = media.handle[i];
        published.loaded[i] = media.loaded[i];
    }
}

static bool bind_pointer(void *destination, size_t capacity, intptr_t address)
{
    _Static_assert(sizeof(intptr_t) == sizeof(void *), "sourced flat 64-bit resolver");
    return address != 0 && capacity == sizeof(address) &&
           memcpy(destination, &address, sizeof(address)) == destination;
}

static bool resolve_lifecycle(void)
{
    if (media.handle[SP5_PS5_MEDIA_GLOBAL_AVCAP2] < 0)
        return false;
    pid_t pid = getpid();
    intptr_t addresses[2];
#if defined(__PROSPERO__)
    addresses[0] = kernel_dynlib_resolve(pid, (uint32_t)media.handle[SP5_PS5_MEDIA_GLOBAL_AVCAP2],
                                         lifecycle_nids[0]);
    addresses[1] = kernel_dynlib_resolve(pid, (uint32_t)media.handle[SP5_PS5_MEDIA_GLOBAL_AVCAP2],
                                         lifecycle_nids[1]);
#else
    (void)pid;
    addresses[0] = 0;
    addresses[1] = 0;
#endif
    return bind_pointer((void *)&media.initialize, sizeof(media.initialize), addresses[0]) &&
           bind_pointer((void *)&media.terminate, sizeof(media.terminate), addresses[1]);
}

static bool load_one(size_t index)
{
    if (media.loaded[index])
        return true;
    uint32_t existing = 0;
    pid_t pid = getpid();
    int rc;
#if defined(__PROSPERO__)
    rc = kernel_dynlib_handle(pid, module_names[index], &existing);
#else
    (void)pid;
    rc = -1;
#endif
    if (rc == 0 && existing != 0 && existing <= (uint32_t)INT32_MAX) {
        media.handle[index] = (int)existing;
        media.loaded[index] = true;
        media.owned[index] = false;
        media.attempted[index] = false;
        media.completed[index] = false;
        return true;
    }
#if defined(__PROSPERO__)
    int result = 0;
    int handle = sceKernelLoadStartModule(module_paths[index], 0, NULL, 0, NULL, &result);
#else
    (void)pid;
    int handle = -1;
    int result = 0;
#endif
    if (handle < 0)
        return false;
    /* A non-null handle from a failed load is adapter-owned and must remain
     * visible so a later unload can release it. */
    media.handle[index] = handle;
    media.loaded[index] = true;
    media.owned[index] = true;
    media.attempted[index] = false;
    media.completed[index] = false;
    return result == 0;
}

/* One bounded unload attempt per close. A failed attempt is retryable by the next close. */
static bool unload_one(size_t index)
{
    if (!media.owned[index] || !media.loaded[index])
        return true;
    if (media.attempted[index])
        return media.completed[index];
    media.attempted[index] = true;
#if defined(__PROSPERO__)
    int result = 0;
    int rc = sceKernelStopUnloadModule(media.handle[index], 0, NULL, 0, NULL, &result);
    media.completed[index] = rc == 0 && result == 0;
#else
    media.completed[index] = true;
#endif
    if (!media.completed[index]) {
        /* A later close may retry the bounded unload; the first failure is
         * retained in media.failure and by the caller's lifecycle diagnostic. */
        media.attempted[index] = false;
        return false;
    }
    media.loaded[index] = false;
    media.owned[index] = false;
    media.handle[index] = -1;
    return true;
}

static sp5_result unload_owned(void)
{
    for (size_t index = SP5_PS5_MEDIA_GLOBAL_MODULE_COUNT; index > 0; --index) {
        if (!media.owned[index - 1])
            continue;
        if (!unload_one(index - 1))
            return sp5_failure(SP5_REASON_PLATFORM);
    }
    return sp5_success();
}

static bool all_loaded(void)
{
    for (size_t i = 0; i < SP5_PS5_MEDIA_GLOBAL_MODULE_COUNT; ++i) {
        if (!media.loaded[i])
            return false;
    }
    return true;
}

static sp5_result terminate_media(void)
{
    if (!media.initialized)
        return sp5_success();
    if (media.terminate == NULL || media.terminate() != 0)
        return sp5_failure(SP5_REASON_PLATFORM);
    media.initialized = false;
    return sp5_success();
}

static sp5_result load_media(void)
{
    if (all_loaded())
        return sp5_success();
    sp5_result cleanup = unload_owned();
    if (cleanup.status != SP5_OK)
        return cleanup;
    for (size_t i = 0; i < SP5_PS5_MEDIA_GLOBAL_MODULE_COUNT; ++i) {
        if (!load_one(i))
            return sp5_failure(SP5_REASON_PLATFORM);
    }
    return sp5_success();
}

/* At zero references: Terminate first, then unload only when the sourced
 * cleanup gate is true. Every failed step remains visible for a later call. */
static sp5_result cleanup_at_zero(void)
{
    if (!media.open)
        return sp5_success();
    sp5_result result = terminate_media();
    if (result.status != SP5_OK) {
        media.failure = result.reason;
        publish();
        return result;
    }
    if (!media.gate_ok) {
        /* The probe keeps modules loaded when a video/resource cleanup failed,
         * even though Terminate was still attempted. */
        media.failure = SP5_REASON_OWNERSHIP;
        publish();
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    result = unload_owned();
    if (result.status != SP5_OK) {
        media.failure = result.reason;
        publish();
        return result;
    }
    media.open = false;
    media.gate_ok = true;
    media.failure = SP5_REASON_NONE;
    publish();
    return sp5_success();
}

sp5_result sp5_ps5_media_global_acquire(void)
{
    if (!sp5_ps5_firmware_supported()) {
        media.failure = SP5_REASON_UNSUPPORTED;
        publish();
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    }
    if (media.refcount != 0) {
        if (media.refcount == UINT32_MAX)
            return sp5_failure(SP5_REASON_OVERFLOW);
        ++media.refcount;
        publish();
        return sp5_success();
    }
    /* A previous release may have stopped at Terminate or unload. Reuse the
     * still-loaded modules rather than double-loading or double-Initialize. */
    sp5_result result = terminate_media();
    if (result.status != SP5_OK) {
        media.failure = result.reason;
        publish();
        return result;
    }
    result = load_media();
    if (result.status != SP5_OK) {
        (void)unload_owned();
        media.failure = result.reason;
        publish();
        return result;
    }
    if (!resolve_lifecycle()) {
        (void)unload_owned();
        media.failure = SP5_REASON_UNSUPPORTED;
        publish();
        return sp5_failure(SP5_REASON_UNSUPPORTED);
    }
    if (media.initialize == NULL || media.initialize() != 0) {
        (void)unload_owned();
        media.failure = SP5_REASON_PLATFORM;
        publish();
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    media.initialized = true;
    media.open = true;
    media.refcount = 1;
    media.gate_ok = true;
    media.failure = SP5_REASON_NONE;
    publish();
    return sp5_success();
}

sp5_result sp5_ps5_media_global_release(void)
{
    if (media.refcount != 0) {
        --media.refcount;
        publish();
        if (media.refcount != 0)
            return sp5_success();
    }
    return cleanup_at_zero();
}

uint32_t sp5_ps5_media_global_refcount(void)
{
    return media.refcount;
}

const sp5_ps5_media_global_info *sp5_ps5_media_global_status(void)
{
    return &published;
}

void sp5_ps5_media_global_note_cleanup(bool ok)
{
    /* Latest cleanup state, not a sticky conjunction: a later close that has
     * actually released its local resources may enable the global unload. */
    media.gate_ok = ok;
}

// NOLINTEND(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
