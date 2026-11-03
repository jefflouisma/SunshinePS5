#include "internal.h"
#include "session/revoke.h"
#include <string.h>
bool sp5_web_confirm(const sp5_json_object *body)
{
    const char *const keys[] = {"confirm"};
    const sp5_json_field *f = sp5_json_get(body, "confirm");
    return body->count == 1 && sp5_json_keys(body, keys, 1) && f && f->type == SP5_JSON_BOOL &&
           f->boolean;
}
bool sp5_web_devices(sp5_web *w, const sp5_request *q, const sp5_json_object *body,
                     sp5_web_response *r)
{
    sp5_devices *devices = sp5_store_devices(w->config.store);
    if (sp5_web_equal(q->method, "GET")) {
        bool ok = sp5_json_raw(&r->body, "{\"operation\":\"devices\",\"devices\":[");
        for (size_t i = 0; i < sp5_devices_count(devices); i++) {
            const sp5_device *d = sp5_devices_at(devices, i);
            ok = ok && (!i || sp5_json_raw(&r->body, ",")) && sp5_json_raw(&r->body, "{\"id\":") &&
                 sp5_web_text(&r->body, d->id) && sp5_json_raw(&r->body, ",\"name\":") &&
                 sp5_web_text(&r->body, d->name) && sp5_json_raw(&r->body, ",\"paired_at\":") &&
                 sp5_json_u64(&r->body, d->paired_at) &&
                 sp5_json_raw(&r->body, ",\"last_session_at\":") &&
                 (d->last_session_at.present ? sp5_json_u64(&r->body, d->last_session_at.value)
                                             : sp5_json_raw(&r->body, "null")) &&
                 sp5_json_raw(&r->body, "}");
        }
        /* A failed durable revocation stays listed until a write succeeds: a
         * restart would otherwise silently restore that device's access. */
        ok = ok && sp5_json_raw(&r->body, "],\"revocation_durability_pending\":") &&
             sp5_json_raw(&r->body, sp5_devices_durability_pending(devices) ? "true" : "false") &&
             sp5_json_raw(&r->body, ",\"pending_revocations\":[");
        for (size_t i = 0; i < sp5_devices_pending_count(devices); i++) {
            const sp5_device_revocation *p = sp5_devices_pending_at(devices, i);
            ok = ok && (!i || sp5_json_raw(&r->body, ",")) && sp5_json_raw(&r->body, "{\"id\":") &&
                 sp5_web_text(&r->body, p->id) && sp5_json_raw(&r->body, ",\"name\":") &&
                 sp5_web_text(&r->body, p->name) && sp5_json_raw(&r->body, "}");
        }
        /* One retained session bounds this list to one item. Registry removal
         * can be durable while resource cleanup still owns an ENDING session. */
        sp5_session_view session = sp5_session_read(w->config.session);
        bool removed = session.device_id[0] != 0;
        for (size_t i = 0; removed && i < sp5_devices_count(devices); i++)
            removed = strcmp(sp5_devices_at(devices, i)->id, session.device_id) != 0;
        ok = ok && sp5_json_raw(&r->body, "],\"cleanup_pending_revocations\":[");
        if (removed && session.state == SP5_SESSION_ENDING && session.has_end_reason)
            ok = ok && sp5_json_raw(&r->body, "{\"id\":") &&
                 sp5_web_text(&r->body, session.device_id) &&
                 sp5_json_raw(&r->body, ",\"name\":") &&
                 sp5_web_text(&r->body, session.device_name) && sp5_json_raw(&r->body, "}");
        return ok && sp5_json_raw(&r->body, "]}");
    }
    if (q->target.size != 80)
        return sp5_web_error(r, 404, "device_not_found", "Device is no longer paired.",
                             "Refresh the device list.", w->request_id);
    const sp5_device *device = NULL;
    for (size_t i = 0; i < sp5_devices_count(devices); i++)
        if (memcmp(sp5_devices_at(devices, i)->id, q->target.data + 16, 64) == 0)
            device = sp5_devices_at(devices, i);
    bool pending = false;
    for (size_t i = 0; !device && i < sp5_devices_pending_count(devices); i++)
        pending =
            pending || memcmp(sp5_devices_pending_at(devices, i)->id, q->target.data + 16, 64) == 0;
    sp5_session_view session = sp5_session_read(w->config.session);
    bool cleanup_pending = session.state == SP5_SESSION_ENDING && session.has_end_reason &&
                           memcmp(session.device_id, q->target.data + 16, 64) == 0;
    if (!device && !(sp5_web_equal(q->method, "DELETE") && (pending || cleanup_pending)))
        return sp5_web_error(r, 404, "device_not_found", "Device is no longer paired.",
                             "Refresh the device list.", w->request_id);
    if (sp5_web_equal(q->method, "PATCH")) {
        const char *const keys[] = {"name"};
        const sp5_json_field *name = sp5_json_get(body, "name");
        if (body->count != 1 || !sp5_json_keys(body, keys, 1) || !name ||
            name->type != SP5_JSON_TEXT ||
            !sp5_name_valid((sp5_bytes){(const uint8_t *)name->text, name->size}))
            return sp5_web_error(r, 400, "invalid_name", sp5_name_requirement(),
                                 "Enter a valid device name.", w->request_id);
        sp5_device_name update = {{device->certificate, device->certificate_size},
                                  {(const uint8_t *)name->text, name->size}};
        if (sp5_devices_rename(devices, update).status != SP5_OK)
            return sp5_web_error(r, 500, "persistence_failed",
                                 "Could not persist the device name; previous name retained.",
                                 "Check storage and retry.", w->request_id);
        return sp5_json_raw(&r->body, "{\"operation\":\"device_rename\",\"state\":\"saved\"}");
    }
    if (!sp5_web_confirm(body))
        return sp5_web_error(r, 400, "confirmation_required", "Revocation requires confirmation.",
                             "Confirm the selected device.", w->request_id);
    char id[65];
    memcpy(id, q->target.data + 16, sizeof(id) - 1);
    id[sizeof(id) - 1] = 0;
    bool durable = false;
    sp5_result result =
        sp5_revoke_device(devices, w->config.session, w->config.lifecycle, id, &durable);
    if (result.status != SP5_OK && result.reason == SP5_REASON_OWNERSHIP)
        return sp5_web_error(r, 500, "revocation_incomplete",
                             "Revocation could not acquire session cleanup ownership.",
                             "Check session cleanup diagnostics before retrying.", w->request_id);
    if (result.status != SP5_OK)
        return sp5_web_error(
            r, 500, "revocation_incomplete",
            durable ? "Removal persisted and access is blocked; session cleanup failed."
                    : "Access is blocked for this process; could not persist revocation. Restart "
                      "protection is not confirmed.",
            "Check storage and cleanup diagnostics before restart; retry safely.", w->request_id);
    return sp5_json_raw(&r->body,
                        "{\"operation\":\"device_revoke\",\"state\":\"revoked\",\"durable\":true}");
}
