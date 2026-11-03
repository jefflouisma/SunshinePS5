#ifndef SUNSHINE_WEB_METRICS_H
#define SUNSHINE_WEB_METRICS_H
#include "server.h"
#include <stdatomic.h>
/* Fixed setup-owned adapter. The caller owns engine lifetime and invokes worker
 * step at least once per second independently of browsers; producers never call
 * web code. Persist requested settings before invoking requested(). */
typedef struct {
    sp5_metrics *engine;
    atomic_bool requested, effective;
    atomic_uint_fast64_t requested_us, applied_us;
    atomic_int diagnostic;
} sp5_metrics_web;
sp5_result sp5_metrics_web_init(sp5_metrics_web *web, sp5_metrics *engine, bool requested,
                                uint64_t now_us);
void sp5_metrics_web_requested(void *context, bool enabled, uint64_t now_us);
bool sp5_metrics_web_effective(void *context);
sp5_result sp5_metrics_web_worker_step(sp5_metrics_web *web, uint64_t now_us);
bool sp5_metrics_web_route(void *context, const sp5_request *request, uint64_t now_us,
                           sp5_web_response *response);
/* Synchronous bounded pull into a backpressured sink, management/export owner
 * only. Failure is final for this download; transport must abort framing rather
 * than claim a complete file. A later fresh request may retry. */
sp5_result sp5_metrics_web_export(void *context, const sp5_metrics_sink *sink);
#endif
