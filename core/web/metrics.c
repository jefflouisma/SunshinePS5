#include "metrics.h"
#include <string.h>
sp5_result sp5_metrics_web_init(sp5_metrics_web *web, sp5_metrics *engine, bool requested,
                                uint64_t now_us)
{
    if (web == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    web->engine = engine;
    atomic_init(&web->requested, requested);
    atomic_init(&web->effective, false);
    atomic_init(&web->requested_us, now_us);
    atomic_init(&web->applied_us, now_us);
    atomic_init(&web->diagnostic, SP5_REASON_NONE);
    if (engine != NULL) {
        sp5_metrics_view view;
        sp5_result result = sp5_metrics_snapshot(engine, &view);
        if (result.status != SP5_OK)
            return result;
        atomic_store(&web->effective, view.metrics_enabled);
    }
    return sp5_success();
}
void sp5_metrics_web_requested(void *context, bool enabled, uint64_t now_us)
{
    sp5_metrics_web *web = context;
    if (web == NULL)
        return;
    atomic_store(&web->requested_us, now_us);
    atomic_store_explicit(&web->requested, enabled, memory_order_release);
}
bool sp5_metrics_web_effective(void *context)
{
    sp5_metrics_web *web = context;
    return web != NULL && atomic_load_explicit(&web->effective, memory_order_acquire);
}
sp5_result sp5_metrics_web_worker_step(sp5_metrics_web *web, uint64_t now_us)
{
    if (web == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    bool requested = atomic_load_explicit(&web->requested, memory_order_acquire);
    if (web->engine == NULL) {
        atomic_store(&web->effective, false);
        atomic_store(&web->diagnostic, requested ? SP5_REASON_STAGE_UNAVAILABLE : SP5_REASON_NONE);
        return sp5_success();
    }
    if (requested != sp5_metrics_web_effective(web)) {
        sp5_result result = sp5_metrics_enable(web->engine, requested, now_us);
        /* Atomic engine state: the worker never waits for a management reader. */
        bool enabled = sp5_metrics_enabled(web->engine);
        atomic_store_explicit(&web->effective, enabled, memory_order_release);
        if (requested == enabled)
            atomic_store(&web->applied_us, now_us);
        atomic_store(&web->diagnostic, result.reason);
        if (result.status != SP5_OK)
            return result;
    }
    return sp5_metrics_worker_step(web->engine, now_us, NULL);
}
sp5_result sp5_metrics_web_export(void *context, const sp5_metrics_sink *sink)
{
    sp5_metrics_web *web = context;
    if (web == NULL || sink == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    return web->engine == NULL ? sp5_failure(SP5_REASON_STAGE_UNAVAILABLE)
                               : sp5_metrics_export(web->engine, sink);
}
static bool equal(sp5_bytes value, const char *text)
{
    return value.size == strlen(text) && value.data != NULL &&
           memcmp(value.data, text, value.size) == 0;
}
bool sp5_metrics_web_route(void *context, const sp5_request *request, uint64_t now_us,
                           sp5_web_response *response)
{
    sp5_metrics_web *web = context;
    (void)now_us; /* Errors carry the dispatch sequence, never a clock value. */
    if (web == NULL || request == NULL || response == NULL)
        return false;
    bool download = equal(request->target, "/api/v1/metrics/export");
    if (!download && !equal(request->target, "/api/v1/metrics"))
        return false;
    if (!equal(request->method, "GET"))
        return sp5_web_error(response, 400, "read_only", "Metrics are read-only here.",
                             "Use the home switch to change collection.", response->request_id);
    response->body.size = 0;
    if (download) {
        if (web->engine == NULL)
            return sp5_web_error(response, 404, "no_capture", "No metrics capture is available.",
                                 "Start a session and try again.", response->request_id);
        response->status = 200;
        response->content_type = "application/x-ndjson";
        response->stream_context = web;
        response->stream = sp5_metrics_web_export;
        response->download_filename = "metrics.ndjson";
        return true;
    }
    bool requested = atomic_load_explicit(&web->requested, memory_order_acquire);
    bool effective = sp5_metrics_web_effective(web);
    bool ok =
        sp5_json_raw(&response->body, "{\"requested\":") &&
        sp5_json_raw(&response->body, requested ? "true" : "false") &&
        sp5_json_raw(&response->body, ",\"effective\":") &&
        sp5_json_raw(&response->body, effective ? "true" : "false") &&
        sp5_json_raw(&response->body, ",\"pending\":") &&
        sp5_json_raw(&response->body, requested != effective ? "true" : "false") &&
        sp5_json_raw(&response->body, ",\"requested_us\":") &&
        sp5_json_u64(&response->body, atomic_load(&web->requested_us)) &&
        sp5_json_raw(&response->body, ",\"applied_us\":") &&
        sp5_json_u64(&response->body, atomic_load(&web->applied_us)) &&
        sp5_json_raw(&response->body, ",\"diagnostic\":") &&
        sp5_json_string(
            &response->body,
            (sp5_bytes){(const uint8_t *)sp5_reason_name((sp5_reason)atomic_load(&web->diagnostic)),
                        strlen(sp5_reason_name((sp5_reason)atomic_load(&web->diagnostic)))}) &&
        sp5_json_raw(&response->body, ",\"capture\":");
    if (ok && web->engine != NULL) {
        sp5_metrics_view view;
        /* The snapshot waits out a worker pass, so failure here is unexpected. */
        sp5_result result = sp5_metrics_snapshot(web->engine, &view);
        if (result.status != SP5_OK)
            return sp5_web_error(
                response, 500, "metrics_failed", "The metrics snapshot could not be read.",
                "Retry; restart SunshinePS5 if this persists.", response->request_id);
        sp5_buffer tail = {response->body.data + response->body.size,
                           response->body.capacity - response->body.size, 0};
        result = sp5_metrics_snapshot_json(&view, &tail);
        if (result.status != SP5_OK)
            return sp5_web_error(
                response, 503, "metrics_capacity", "The metrics snapshot could not fit.",
                "Retry with the supported response capacity.", response->request_id);
        response->body.size += tail.size;
    } else if (ok)
        ok = sp5_json_raw(&response->body, "null");
    if (!ok || !sp5_json_raw(&response->body, "}"))
        return sp5_web_error(response, 503, "metrics_capacity",
                             "The metrics snapshot could not fit.",
                             "Retry with the supported response capacity.", response->request_id);
    response->status = 200;
    response->content_type = "application/json";
    return true;
}
