#ifndef SUNSHINE_SESSION_LIFECYCLE_H
#define SUNSHINE_SESSION_LIFECYCLE_H
#include "../lifecycle.h"
#include "controller.h"
#include "metrics/records.h"

/* Setup-only policy, copied before start. Production capabilities and resource
 * costs need measured evidence. Synthetic policy is useful only with a synthetic
 * system probe. It never establishes PS5 modes, budgets or scheduling safety. */
typedef struct {
    sp5_host_mode modes[SP5_MODE_CAPACITY];
    size_t mode_count;
    size_t media_bytes, core_bytes;
    uint64_t streaming_cpu_us_per_10s, idle_cpu_us_per_10s;
    sp5_validity validity;
    bool synthetic;
    const char *evidence_ref;
    sp5_clock_ops clock;
    sp5_metrics_capacity metrics_capacity;
    sp5_metrics_context metrics_context;
    bool metrics_enabled;
} sp5_lifecycle_policy;
sp5_result sp5_lifecycle_configure(sp5_lifecycle *lifecycle, const sp5_lifecycle_policy *policy);
/* Binding is setup-only, after admission and before start. Unbound legacy
 * lifecycle is a trusted synthetic adapter entrypoint, never network admission. */
sp5_result sp5_lifecycle_bind(sp5_lifecycle *lifecycle, sp5_session *session,
                              sp5_bytes verified_certificate);
sp5_metrics *sp5_lifecycle_metrics(sp5_lifecycle *lifecycle);
sp5_bytes sp5_lifecycle_pool(const sp5_lifecycle *lifecycle);
/* Borrow a bounded arena slice for the sole media owner; no allocation. The
 * owner serializes/reuses slices and returns every borrow before end/destroy. */
sp5_result sp5_lifecycle_media_buffer(sp5_lifecycle *lifecycle, size_t offset, sp5_buffer *buffer,
                                      size_t capacity);
/* Independent input owner calls stamp at actual receipt BEFORE transport parse
 * or authentication, then receive only for authenticated control input. No media
 * lock, queue, allocation, export, worker or network send occurs in receive. */
sp5_result sp5_lifecycle_input(sp5_lifecycle *lifecycle, const sp5_control_event *packet,
                               sp5_timestamp received, uint64_t event_id,
                               sp5_input_evidence *evidence);
/* Same input owner: invalidate admission and neutralize/remove controllers before
 * waiting for media. Then quiesce producers and call end on the management owner.
 * All management APIs/read are serialized; destroy requires all owners quiescent.
 * Media adapters must return leases before end closes them. */
sp5_result sp5_lifecycle_stop_input(sp5_lifecycle *lifecycle, sp5_end_reason reason);
bool sp5_lifecycle_input_open(const sp5_lifecycle *lifecycle);
sp5_reason sp5_lifecycle_cleanup_diagnostic(const sp5_lifecycle *lifecycle);
const char *sp5_lifecycle_explanation(const sp5_lifecycle *lifecycle);
#endif
