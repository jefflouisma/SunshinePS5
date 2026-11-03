#include "internal.h"
#include <string.h>
bool sp5_web_settings(sp5_web *w, const sp5_json_object *body, uint64_t now, sp5_web_response *r)
{
    const char *const keys[] = {"host_name", "metrics_enabled"};
    const sp5_json_field *name = sp5_json_get(body, "host_name"),
                         *metrics = sp5_json_get(body, "metrics_enabled");
    if (!body->count || !sp5_json_keys(body, keys, 2) ||
        (name && (name->type != SP5_JSON_TEXT ||
                  !sp5_name_valid((sp5_bytes){(const uint8_t *)name->text, name->size}))) ||
        (metrics && metrics->type != SP5_JSON_BOOL))
        return sp5_web_error(r, 400, "invalid_settings", sp5_name_requirement(),
                             "Correct the name or metrics value and retry.", w->request_id);
    sp5_settings settings = sp5_store_settings(w->config.store);
    if (name) {
        memset(settings.host_name, 0, sizeof(settings.host_name));
        memcpy(settings.host_name, name->text, name->size);
    }
    if (metrics)
        settings.metrics_enabled = metrics->boolean;
    if (sp5_store_save_settings(w->config.store, &settings).status != SP5_OK)
        return sp5_web_error(r, 500, "persistence_failed",
                             "Could not persist settings; previous settings retained.",
                             "Check storage and retry.", w->request_id);
    if (sp5_host_settings(w->config.host, &settings).status != SP5_OK)
        return sp5_web_error(r, 500, "settings_failed", "Saved settings could not be published.",
                             "Restart and reload saved settings.", w->request_id);
    if (metrics && w->config.extensions.settings_requested)
        w->config.extensions.settings_requested(w->config.extensions.context,
                                                settings.metrics_enabled, now);
    bool effective = w->config.extensions.metrics_effective &&
                     w->config.extensions.metrics_effective(w->config.extensions.context);
    return sp5_json_raw(&r->body,
                        "{\"operation\":\"settings\",\"state\":\"saved\",\"host_name\":") &&
           sp5_web_text(&r->body, settings.host_name) &&
           sp5_json_raw(&r->body, ",\"metrics_enabled\":") &&
           sp5_json_raw(&r->body, settings.metrics_enabled ? "true" : "false") &&
           sp5_json_raw(&r->body, ",\"metrics_effective\":") &&
           sp5_json_raw(&r->body, effective ? "true" : "false") && sp5_json_raw(&r->body, "}");
}
