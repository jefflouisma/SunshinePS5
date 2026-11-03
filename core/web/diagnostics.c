#include "diagnostics.h"
#include <limits.h>
#include <string.h>
static sp5_bytes text(const char *value)
{
    return (sp5_bytes){(const uint8_t *)value, strlen(value)};
}
static bool same(sp5_bytes value, const char *literal)
{
    return value.data != NULL && value.size == strlen(literal) &&
           memcmp(value.data, literal, value.size) == 0;
}
sp5_result sp5_diagnostics_init(sp5_diagnostics *d, sp5_metrics *metrics, const char *host,
                                const char *firmware)
{
    if (d == NULL || host == NULL || firmware == NULL || host[0] == 0 || firmware[0] == 0 ||
        strlen(host) >= sizeof(d->host_version) || strlen(firmware) >= sizeof(d->firmware_version))
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *d = (sp5_diagnostics){.metrics = metrics};
    memcpy(d->host_version, host, strlen(host) + 1);
    memcpy(d->firmware_version, firmware, strlen(firmware) + 1);
    return sp5_success();
}
sp5_result sp5_diagnostics_log(sp5_diagnostics *d, uint64_t at_us, uint64_t session_id,
                               sp5_log_event event, sp5_reason reason)
{
    if (d == NULL || event < SP5_LOG_SESSION_END || event >= SP5_LOG_EVENT_INVALID ||
        reason < SP5_REASON_NONE || reason > SP5_REASON_OWNERSHIP)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (d->log_count == SP5_DIAGNOSTIC_LOG_CAPACITY) {
        if (d->logs_evicted == UINT64_MAX)
            return sp5_failure(SP5_REASON_OVERFLOW);
        ++d->logs_evicted;
        d->log_head = (d->log_head + 1) % SP5_DIAGNOSTIC_LOG_CAPACITY;
    } else
        ++d->log_count;
    size_t tail = (d->log_head + d->log_count - 1) % SP5_DIAGNOSTIC_LOG_CAPACITY;
    d->logs[tail] = (sp5_public_log){at_us, session_id, event, reason};
    return sp5_success();
}
sp5_result sp5_diagnostics_end(sp5_diagnostics *d, sp5_session_end_record record)
{
    if (d == NULL || record.session_id == 0 || record.reason < SP5_END_CLIENT_QUIT ||
        record.reason > SP5_END_ERROR || record.diagnostic < SP5_REASON_NONE ||
        record.diagnostic > SP5_REASON_OWNERSHIP)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    for (size_t i = 0; i < d->end_count; ++i) {
        const sp5_session_end_record *old =
            &d->ends[(d->end_head + i) % SP5_DIAGNOSTIC_END_CAPACITY];
        if (old->session_id == record.session_id)
            return old->reason == record.reason && old->diagnostic == record.diagnostic
                       ? sp5_success()
                       : sp5_failure(SP5_REASON_OWNERSHIP);
    }
    if (d->end_count == SP5_DIAGNOSTIC_END_CAPACITY) {
        if (d->ends_evicted == UINT64_MAX)
            return sp5_failure(SP5_REASON_OVERFLOW);
        ++d->ends_evicted;
        d->end_head = (d->end_head + 1) % SP5_DIAGNOSTIC_END_CAPACITY;
    } else
        ++d->end_count;
    d->ends[(d->end_head + d->end_count - 1) % SP5_DIAGNOSTIC_END_CAPACITY] = record;
    return sp5_success();
}
static const char *end_name(sp5_end_reason reason)
{
    static const char *const names[] = {"client_quit",   "client_lost",      "revoked",
                                        "host_stopping", "console_conflict", "error"};
    return reason <= SP5_END_ERROR ? names[reason] : "invalid";
}
sp5_result sp5_diagnostics_export(void *context, const sp5_metrics_sink *sink)
{
    const sp5_diagnostics *d = context;
    if (d == NULL || sink == NULL || sink->write == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    uint8_t data[16384];
    sp5_buffer out = {data, sizeof(data), 0};
    bool ok =
        sp5_json_raw(&out,
                     "{\"kind\":\"diagnostics\",\"schema_version\":\"1.0.0\",\"host_version\":") &&
        sp5_json_string(&out, text(d->host_version)) &&
        sp5_json_raw(&out, ",\"firmware_version\":") &&
        sp5_json_string(&out, text(d->firmware_version)) &&
        sp5_json_raw(&out, ",\"capture_available\":") &&
        sp5_json_raw(&out, d->metrics != NULL ? "true" : "false") &&
        sp5_json_raw(&out, ",\"logs_evicted\":") && sp5_json_u64(&out, d->logs_evicted) &&
        sp5_json_raw(&out, ",\"end_reasons_evicted\":") && sp5_json_u64(&out, d->ends_evicted) &&
        sp5_json_raw(&out, ",\"logs\":[");
    for (size_t i = 0; i < d->log_count && ok; ++i) {
        const sp5_public_log *entry = &d->logs[(d->log_head + i) % SP5_DIAGNOSTIC_LOG_CAPACITY];
        ok = (i == 0 || sp5_json_raw(&out, ",")) && sp5_json_raw(&out, "{\"at_us\":") &&
             sp5_json_u64(&out, entry->at_us) && sp5_json_raw(&out, ",\"session_id\":") &&
             sp5_json_u64(&out, entry->session_id) && sp5_json_raw(&out, ",\"event\":") &&
             sp5_json_string(
                 &out, text(entry->event == SP5_LOG_SESSION_END ? "session_end" : "diagnostic")) &&
             sp5_json_raw(&out, ",\"reason\":") &&
             sp5_json_string(&out, text(sp5_reason_name(entry->reason))) && sp5_json_raw(&out, "}");
    }
    ok = ok && sp5_json_raw(&out, "],\"session_ends\":[");
    for (size_t i = 0; i < d->end_count && ok; ++i) {
        const sp5_session_end_record *entry =
            &d->ends[(d->end_head + i) % SP5_DIAGNOSTIC_END_CAPACITY];
        ok = (i == 0 || sp5_json_raw(&out, ",")) && sp5_json_raw(&out, "{\"session_id\":") &&
             sp5_json_u64(&out, entry->session_id) && sp5_json_raw(&out, ",\"reason\":") &&
             sp5_json_string(&out, text(end_name(entry->reason))) &&
             sp5_json_raw(&out, ",\"diagnostic\":") &&
             sp5_json_string(&out, text(sp5_reason_name(entry->diagnostic))) &&
             sp5_json_raw(&out, "}");
    }
    if (!ok || !sp5_json_raw(&out, "]}\n"))
        return sp5_failure(SP5_REASON_CAPACITY);
    sp5_result result = sink->write(sink->context, (sp5_bytes){out.data, out.size});
    if (result.status == SP5_OK && d->metrics != NULL)
        result = sp5_metrics_export(d->metrics, sink);
    return result;
}
bool sp5_diagnostics_route(void *context, const sp5_request *request, uint64_t now_us,
                           sp5_web_response *response)
{
    (void)now_us; /* Errors carry the dispatch sequence, never a clock value. */
    if (context == NULL || request == NULL || response == NULL ||
        !same(request->target, "/api/v1/diagnostics"))
        return false;
    if (!same(request->method, "GET"))
        return sp5_web_error(response, 400, "read_only", "Diagnostics are a read-only download.",
                             "Use the download button.", response->request_id);
    response->status = 200;
    response->content_type = "application/x-ndjson";
    response->body.size = 0;
    response->stream = sp5_diagnostics_export;
    response->stream_context = context;
    response->download_filename = "diagnostics.ndjson";
    return true;
}
