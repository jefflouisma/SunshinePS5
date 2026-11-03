#ifndef SUNSHINE_PLATFORM_PS5_MEDIA_GLOBAL_H
#define SUNSHINE_PLATFORM_PS5_MEDIA_GLOBAL_H

#include "platform.h"

#include <stdint.h>

/* Process-global Avcap2 lifetime owner for the provisional console adapters.
 *
 * The retained T041 probe loads Ipmi, ResourceArbitrator and Avcap2, calls
 * Avcap2 Initialize exactly once, and tears them down in reverse order. This
 * module owns exactly that lifetime so capture, audio and other media clients
 * cannot double-initialize or unload a module still in use. There is NO
 * allocation and NO locking: callers must serialize all media lifetime use
 * (documented single media owner). Reference counting is not a concurrency
 * primitive.
 *
 * Acquire loads all three modules if needed, resolves and calls Initialize once,
 * then increments the reference count. Release decrements it and, only at zero,
 * calls Terminate and unloads adapter-owned modules. A failed cleanup remains
 * visible and retryable through a later release; a module unload that the probe
 * would not retry stays sticky. */

enum {
    SP5_PS5_MEDIA_GLOBAL_MODULE_COUNT = 3,
    SP5_PS5_MEDIA_GLOBAL_IPMI = 0,
    SP5_PS5_MEDIA_GLOBAL_RESOURCE = 1,
    SP5_PS5_MEDIA_GLOBAL_AVCAP2 = 2
};

typedef struct {
    int handle[SP5_PS5_MEDIA_GLOBAL_MODULE_COUNT];
    bool loaded[SP5_PS5_MEDIA_GLOBAL_MODULE_COUNT];
    bool initialized;
    sp5_reason failure;
    uint32_t refcount;
} sp5_ps5_media_global_info;

sp5_result sp5_ps5_media_global_acquire(void);
sp5_result sp5_ps5_media_global_release(void);
uint32_t sp5_ps5_media_global_refcount(void);

/* Read-only handles/status for adapters that must resolve their own exports.
 * Valid after a successful acquire while the caller holds a reference. */
const sp5_ps5_media_global_info *sp5_ps5_media_global_status(void);

/* Capture calls this immediately before release with the sourced cleanup gate:
 * false means Terminate is still attempted but adapter-owned module unload is
 * skipped, matching the probe's gated module-cleanup rule. The gate reflects
 * the latest close so a later close that has released local resources can
 * unload; a successful acquire resets it. */
void sp5_ps5_media_global_note_cleanup(bool ok);

#endif
