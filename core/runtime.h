#ifndef SUNSHINE_CORE_RUNTIME_H
#define SUNSHINE_CORE_RUNTIME_H

#include "platform.h"

sp5_result sp5_success(void);
sp5_result sp5_failure(sp5_reason reason);
const char *sp5_reason_name(sp5_reason reason);

/* Borrowed storage; operations are transactional on error and accept overlap.
 * Not a concurrent queue: the owner serializes access. Reset retains storage. */
sp5_result sp5_buffer_init(sp5_buffer *buffer, uint8_t *storage, size_t capacity);
sp5_result sp5_buffer_append(sp5_buffer *buffer, sp5_bytes bytes);
sp5_result sp5_buffer_reset(sp5_buffer *buffer);

/* No clock reads or allocation. Description uncertainty is per timestamp;
 * elapsed-time uncertainty is the conservative sum for the two endpoints.
 * Unknown clock uncertainty stays absent. Conversion quantization is separate.
 * Every failure clears numeric output, including after a preceding success. */
sp5_result sp5_duration_between(const sp5_clock_description *clock, sp5_timestamp start,
                                sp5_timestamp end, sp5_duration *duration);

typedef enum { SP5_LOG_SESSION_END = 0, SP5_LOG_DIAGNOSTIC, SP5_LOG_EVENT_INVALID } sp5_log_event;
typedef enum { SP5_LOG_SECRET = 0, SP5_LOG_PUBLIC, SP5_LOG_VISIBILITY_INVALID } sp5_log_visibility;
enum { SP5_LOG_PUBLIC_LIMIT = 64, SP5_LOG_RECORD_CAPACITY = 192 };

/* Secret is the default; secret bytes are never dereferenced. Only explicitly
 * public troubleshooting context may be copied. No printf-style formatting.
 * Caller owns bounded retention and event sampling; never log every frame.
 * Appends bytes, not a NUL terminator. Full storage returns CAPACITY, not eviction. */
sp5_result sp5_log_write(sp5_buffer *buffer, sp5_log_event event, sp5_reason reason,
                         sp5_log_visibility visibility, sp5_bytes detail);

#endif
