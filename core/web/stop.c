#include "internal.h"
bool sp5_web_stop(sp5_web *w, const sp5_json_object *body, sp5_web_response *r)
{
    if (!sp5_web_confirm(body))
        return sp5_web_error(r, 400, "confirmation_required", "Stopping requires confirmation.",
                             "Confirm stopping the host.", w->request_id);
    if (sp5_host_transition(w->config.host, SP5_HOST_STOPPING).status != SP5_OK)
        return sp5_web_error(r, 409, "stop_conflict", "Cannot begin stopping.",
                             "Refresh host status.", w->request_id);
    /* Closing input is independent of acknowledgement and media cleanup. The
     * transport flushes this response before quiescing media and calling finish. */
    if (w->config.session && sp5_session_read(w->config.session).state != SP5_SESSION_ENDED &&
        sp5_session_begin_end(w->config.session, SP5_END_HOST_STOPPING, SP5_REASON_NONE).status !=
            SP5_OK)
        return sp5_web_error(r, 500, "stop_failed", "Could not close session admission.",
                             "Check cleanup diagnostics.", w->request_id);
    if (w->config.lifecycle &&
        sp5_lifecycle_stop_input(w->config.lifecycle, SP5_END_HOST_STOPPING).status != SP5_OK)
        return sp5_web_error(r, 500, "input_cleanup_failed",
                             "Stopping; input cleanup reported a failure.",
                             "Check cleanup diagnostics.", w->request_id);
    r->stopping = true;
    return sp5_json_raw(&r->body, "{\"operation\":\"host_stop\",\"state\":\"stopping\"}");
}
sp5_result sp5_web_finish_stop(sp5_web *w)
{
    if (!w || w->config.host->state != SP5_HOST_STOPPING)
        return sp5_failure(SP5_REASON_CONFLICT);
    if (w->config.lifecycle)
        return sp5_lifecycle_end(w->config.lifecycle, SP5_END_HOST_STOPPING);
    return w->config.session && sp5_session_read(w->config.session).state != SP5_SESSION_ENDED
               ? sp5_session_end(w->config.session, SP5_END_HOST_STOPPING)
               : sp5_success();
}
