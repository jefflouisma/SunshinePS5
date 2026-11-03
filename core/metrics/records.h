#ifndef SUNSHINE_METRICS_RECORDS_H
#define SUNSHINE_METRICS_RECORDS_H

#include "../metrics.h"

/* One producer owns each queue. Management/worker/export calls are serialized
 * internally; no producer takes a lock. All callers quiesce before destroy.
 * stamp() is called at input receipt BEFORE parsing/authentication; record_input
 * is called only after authority checks and the actual delivery attempt. */
enum {
    SP5_METRICS_FRAME_CAPACITY = 131072,
    SP5_METRICS_INPUT_CAPACITY = 2097152,
    SP5_METRICS_QUEUE_CAPACITY = 16384,
    SP5_METRICS_INTERVAL_CAPACITY = 128,
    SP5_INPUT_AUTHENTICATED = 4,
    SP5_INPUT_FAILED = 8,
    SP5_INPUT_DURATION_CLOCK_INVALID = 16,
    SP5_RECORD_CLOCK_INVALID = 64,
    SP5_FRAME_OUTCOME_SHIFT = 8,
    SP5_RECORD_REASON_SHIFT = 16,
    SP5_RECORD_STAGE_CLOCK_SHIFT = 32
};
sp5_result sp5_metrics_record_frame(sp5_metrics *metrics, const sp5_frame_evidence *evidence);
sp5_result sp5_metrics_record_input(sp5_metrics *metrics, const sp5_input_evidence *evidence,
                                    bool authenticated);

#endif
