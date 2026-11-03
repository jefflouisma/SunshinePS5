#include "internal.h"
#include <string.h>
bool sp5_web_text(sp5_buffer *out, const char *text)
{
    return sp5_json_string(out, (sp5_bytes){(const uint8_t *)text, strlen(text)});
}
bool sp5_web_status(sp5_web *web, uint64_t now_us, sp5_web_response *response)
{
    sp5_host *host = web->config.host;
    sp5_settings settings = sp5_store_settings(web->config.store);
    sp5_session_view s = sp5_session_read(web->config.session);
    sp5_buffer *out = &response->body;
    bool effective = web->config.extensions.metrics_effective &&
                     web->config.extensions.metrics_effective(web->config.extensions.context);
    bool ok = sp5_json_raw(out, "{\"operation\":\"status\",\"host\":{\"name\":") &&
              sp5_web_text(out, settings.host_name) && sp5_json_raw(out, ",\"state\":") &&
              sp5_web_text(out, host->state == SP5_HOST_RUNNING    ? "running"
                                : host->state == SP5_HOST_STOPPING ? "stopping"
                                                                   : "starting") &&
              sp5_json_raw(out, ",\"version\":") && sp5_web_text(out, host->version) &&
              sp5_json_raw(out, ",\"firmware_version\":") &&
              sp5_web_text(out, host->firmware_version) && sp5_json_raw(out, ",\"address\":") &&
              sp5_web_text(out, host->address) &&
              sp5_json_raw(out, "},\"settings\":{\"host_name\":") &&
              sp5_web_text(out, settings.host_name) && sp5_json_raw(out, ",\"metrics_enabled\":") &&
              sp5_json_raw(out, settings.metrics_enabled ? "true" : "false") &&
              sp5_json_raw(out, ",\"metrics_effective\":") &&
              sp5_json_raw(out, effective ? "true" : "false") && sp5_json_raw(out, "},");
    /* FR-039: unreadable settings/devices and unsaved revocations stay visible
     * with a next step; reason names are fixed vocabulary, never raw bytes. */
    sp5_store_view storage = sp5_store_read(web->config.store);
    sp5_devices *registry = sp5_store_devices(web->config.store);
    ok = ok && sp5_json_raw(out, "\"storage\":{\"settings_recovered\":") &&
         sp5_json_raw(out, storage.settings_recovered ? "true" : "false") &&
         sp5_json_raw(out, ",\"repairing_required\":") &&
         sp5_json_raw(out, storage.repairing_required ? "true" : "false") &&
         sp5_json_raw(out, ",\"revocation_durability_pending\":") &&
         sp5_json_raw(out, sp5_devices_durability_pending(registry) ? "true" : "false") &&
         sp5_json_raw(out, ",\"diagnostic\":") &&
         sp5_web_text(out, sp5_reason_name(storage.diagnostic)) &&
         sp5_json_raw(out, ",\"explanation\":") &&
         sp5_web_text(out, sp5_store_explanation(web->config.store)) &&
         sp5_json_raw(out, "},\"session\":");
    if (!web->config.session || s.state == SP5_SESSION_ENDED)
        return ok && sp5_json_raw(out, "null}");
    const sp5_device *device = NULL;
    sp5_devices *devices = sp5_store_devices(web->config.store);
    for (size_t i = 0; i < sp5_devices_count(devices); i++)
        if (strcmp(sp5_devices_at(devices, i)->id, s.device_id) == 0)
            device = sp5_devices_at(devices, i);
    const char *adjustment = s.bitrate_adjustment == SP5_BITRATE_RAISED    ? "raised_to_minimum"
                             : s.bitrate_adjustment == SP5_BITRATE_LOWERED ? "lowered_to_maximum"
                                                                           : "none";
    ok = ok && sp5_json_raw(out, "{\"id\":\"") && sp5_json_u64(out, s.id) &&
         sp5_json_raw(out, "\",\"state\":") &&
         sp5_web_text(out, s.state == SP5_SESSION_ACTIVE   ? "active"
                           : s.state == SP5_SESSION_ENDING ? "ending"
                                                           : "negotiating") &&
         sp5_json_raw(out, ",\"device_name\":") &&
         sp5_web_text(out, device ? device->name : s.device_name) &&
         sp5_json_raw(out, ",\"width\":") && sp5_json_u64(out, s.width) &&
         sp5_json_raw(out, ",\"height\":") && sp5_json_u64(out, s.height) &&
         sp5_json_raw(out, ",\"fps\":") && sp5_json_u64(out, s.fps) &&
         sp5_json_raw(out, ",\"codec\":") && sp5_web_text(out, s.codec) &&
         sp5_json_raw(out, ",\"dynamic_range\":") &&
         sp5_web_text(out, s.dynamic_range == SP5_SDR ? "sdr" : "hdr") &&
         sp5_json_raw(out, ",\"requested_bitrate_kbps\":") &&
         sp5_json_u64(out, s.requested_bitrate_kbps) &&
         sp5_json_raw(out, ",\"chosen_bitrate_kbps\":") &&
         sp5_json_u64(out, s.chosen_bitrate_kbps) &&
         sp5_json_raw(out, ",\"bitrate_adjustment\":") && sp5_web_text(out, adjustment) &&
         sp5_json_raw(out, ",\"duration_us\":");
    ok = ok &&
         (now_us >= s.started_us ? sp5_json_u64(out, now_us - s.started_us)
                                 : sp5_json_raw(out, "null")) &&
         sp5_json_raw(out, ",\"measured_bitrate_bps\":") &&
         (s.measured_bitrate_bps.present ? sp5_json_u64(out, s.measured_bitrate_bps.value)
                                         : sp5_json_raw(out, "null")) &&
         sp5_json_raw(out, ",\"measured_interval_start_us\":") &&
         (s.measured_bitrate_bps.present ? sp5_json_u64(out, s.measured_interval_start_us)
                                         : sp5_json_raw(out, "null")) &&
         sp5_json_raw(out, ",\"measured_interval_end_us\":") &&
         (s.measured_bitrate_bps.present ? sp5_json_u64(out, s.measured_interval_end_us)
                                         : sp5_json_raw(out, "null"));
    return ok && sp5_json_raw(out, "}}");
}
