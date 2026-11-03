#include "revoke.h"
#include "session.h"
#include <string.h>
sp5_result sp5_revoke_device(sp5_devices *devices, sp5_session *session, sp5_lifecycle *lifecycle,
                             const char *id, bool *persisted)
{
    if (persisted)
        *persisted = false;
    if (!devices || !id)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    const sp5_device *d = NULL;
    for (size_t i = 0; i < sp5_devices_count(devices); i++)
        if (strcmp(sp5_devices_at(devices, i)->id, id) == 0)
            d = sp5_devices_at(devices, i);
    bool pending = false;
    for (size_t i = 0; i < sp5_devices_pending_count(devices); i++)
        pending = pending || strcmp(sp5_devices_pending_at(devices, i)->id, id) == 0;
    sp5_session_view view = sp5_session_read(session);
    bool matching = session && view.state != SP5_SESSION_ENDED && strcmp(view.device_id, id) == 0;
    /* The registry entry disappears before a failed close can be retried. Only
     * the retained ending session or a pending durable removal owns that retry.
     * A prior end reason (for example client loss) must remain the first reason. */
    bool retry = matching && view.state == SP5_SESSION_ENDING && view.has_end_reason;
    if (!d && !pending && !retry)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    if (matching && lifecycle && sp5_lifecycle_read(lifecycle).id != view.id)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    uint8_t certificate[SP5_CERTIFICATE_CAPACITY];
    size_t size = d ? d->certificate_size : 0;
    if (d)
        memcpy(certificate, d->certificate, size);
    sp5_result cleanup = sp5_success();
    if (matching) {
        cleanup = sp5_session_begin_end(session, SP5_END_REVOKED, SP5_REASON_NONE);
        if (lifecycle) {
            sp5_result input = sp5_lifecycle_stop_input(lifecycle, SP5_END_REVOKED);
            sp5_result end = sp5_lifecycle_end(lifecycle, SP5_END_REVOKED);
            if (input.status != SP5_OK)
                cleanup = input;
            else if (end.status != SP5_OK)
                cleanup = end;
        } else {
            sp5_result end = sp5_session_end(session, SP5_END_REVOKED);
            if (end.status != SP5_OK)
                cleanup = end;
        }
    }
    /* Lifecycle admission/input is closed before storage can stall. Registry
     * invalidation also rejects every retained lease and later request. */
    sp5_result durable = d         ? sp5_devices_revoke(devices, (sp5_bytes){certificate, size})
                         : pending ? sp5_devices_persist_pending(devices)
                                   : sp5_success();
    if (persisted)
        *persisted = durable.status == SP5_OK;
    return durable.status != SP5_OK ? durable : cleanup;
}
