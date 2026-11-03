#ifndef SUNSHINE_CORE_LIFECYCLE_H
#define SUNSHINE_CORE_LIFECYCLE_H

#include "protocol.h"

typedef struct sp5_lifecycle sp5_lifecycle;
typedef struct {
    sp5_capture_ops capture;
    sp5_encode_ops encoder;
    sp5_audio_ops audio;
    sp5_input_ops input;
    sp5_system_ops system;
} sp5_lifecycle_ops;
/* Setup-only allocation. All successful opens unwind; end first refuses input
 * and releases/removes controllers, then closes media and rolls back hooks.
 * Teardown is idempotent, retains its first end reason, reports cleanup errors.
 * No adapter operation may stop the foreground game. */
SP5_CORE_API sp5_lifecycle *sp5_lifecycle_create(uint64_t session_id, const sp5_lifecycle_ops *ops);
SP5_CORE_API void sp5_lifecycle_destroy(sp5_lifecycle *lifecycle);
SP5_CORE_API sp5_result sp5_lifecycle_start(sp5_lifecycle *lifecycle,
                                            const sp5_media_config *config, uint64_t now_us);
SP5_CORE_API sp5_result sp5_lifecycle_contact(sp5_lifecycle *lifecycle, uint64_t now_us);
SP5_CORE_API sp5_result sp5_lifecycle_tick(sp5_lifecycle *lifecycle, uint64_t now_us,
                                           bool detected_loss);
SP5_CORE_API sp5_result sp5_lifecycle_end(sp5_lifecycle *lifecycle, sp5_end_reason reason);
SP5_CORE_API sp5_session_view sp5_lifecycle_read(const sp5_lifecycle *lifecycle);

#endif
