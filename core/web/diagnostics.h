#ifndef SUNSHINE_WEB_DIAGNOSTICS_H
#define SUNSHINE_WEB_DIAGNOSTICS_H
#include "metrics.h"
enum { SP5_DIAGNOSTIC_LOG_CAPACITY = 64, SP5_DIAGNOSTIC_END_CAPACITY = 16 };
typedef struct {
    uint64_t at_us, session_id;
    sp5_log_event event;
    sp5_reason reason;
} sp5_public_log;
/* Only structured PUBLIC enum/numeric data enters logs. No arbitrary request,
 * memory, credential or user-controlled log text is accepted by this API.
 * Build/firmware version metadata must come from trusted host setup. Serialized
 * management owner; producers never log/export through this object. */
typedef struct {
    sp5_metrics *metrics;
    sp5_public_log logs[SP5_DIAGNOSTIC_LOG_CAPACITY];
    sp5_session_end_record ends[SP5_DIAGNOSTIC_END_CAPACITY];
    size_t log_head, log_count, end_head, end_count;
    uint64_t logs_evicted, ends_evicted;
    char host_version[64], firmware_version[64];
} sp5_diagnostics;
sp5_result sp5_diagnostics_init(sp5_diagnostics *diagnostics, sp5_metrics *metrics,
                                const char *host_version, const char *firmware_version);
sp5_result sp5_diagnostics_log(sp5_diagnostics *diagnostics, uint64_t at_us, uint64_t session_id,
                               sp5_log_event event, sp5_reason reason);
sp5_result sp5_diagnostics_end(sp5_diagnostics *diagnostics, sp5_session_end_record record);
sp5_result sp5_diagnostics_export(void *context, const sp5_metrics_sink *sink);
bool sp5_diagnostics_route(void *context, const sp5_request *request, uint64_t now_us,
                           sp5_web_response *response);
#endif
