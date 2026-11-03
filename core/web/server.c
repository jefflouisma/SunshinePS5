#include "assets.h"
#include "internal.h"
#include "protocol/http.h"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool sp5_web_equal(sp5_bytes value, const char *text)
{
    return value.size == strlen(text) && memcmp(value.data, text, value.size) == 0;
}
bool sp5_web_error(sp5_web_response *r, unsigned status, const char *code, const char *message,
                   const char *recovery, uint64_t id)
{
    r->status = status;
    r->content_type = "application/json";
    r->body.size = 0;
    r->stopping = false;
    r->stream = NULL;
    r->stream_context = NULL;
    r->download_filename = NULL;
    return sp5_json_raw(&r->body, "{\"error\":{\"code\":") && sp5_web_text(&r->body, code) &&
           sp5_json_raw(&r->body, ",\"message\":") && sp5_web_text(&r->body, message) &&
           sp5_json_raw(&r->body, ",\"recovery\":") && sp5_web_text(&r->body, recovery) &&
           sp5_json_raw(&r->body, ",\"request_id\":") && sp5_json_u64(&r->body, id) &&
           sp5_json_raw(&r->body, "}}");
}
sp5_web *sp5_web_create(const sp5_web_config *c)
{
    if (!c || !c->host || !c->store || !c->authority || strlen(c->authority) == 0 ||
        strlen(c->authority) >= 192)
        return NULL;
    for (const char *p = c->authority; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              strchr(".:-[]", *p)))
            return NULL;
    sp5_web *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->config = *c;
    memcpy(w->authority, c->authority, strlen(c->authority) + 1);
    w->config.authority = w->authority;
    int n = snprintf(w->origin, sizeof(w->origin), "%s://%s", c->https ? "https" : "http",
                     c->authority);
    if (n < 0 || (size_t)n >= sizeof(w->origin)) {
        free(w);
        return NULL;
    }
    return w;
}
void sp5_web_destroy(sp5_web *w)
{
    if (w) {
        OPENSSL_cleanse(w, sizeof(*w));
        free(w);
    }
}
static bool header(sp5_bytes raw, const char *key, sp5_bytes *value)
{
    *value = (sp5_bytes){0};
    bool found = false;
    size_t at = 0;
    while (at + 1 < raw.size && !(raw.data[at] == '\r' && raw.data[at + 1] == '\n'))
        at++;
    at += 2;
    while (at + 1 < raw.size) {
        size_t end = at;
        while (end + 1 < raw.size && !(raw.data[end] == '\r' && raw.data[end + 1] == '\n'))
            end++;
        if (end == at)
            break;
        size_t colon = at;
        while (colon < end && raw.data[colon] != ':')
            colon++;
        bool match = colon - at == strlen(key);
        for (size_t i = at; match && i < colon; i++) {
            uint8_t c = raw.data[i];
            if (c >= 'A' && c <= 'Z')
                c = (uint8_t)(c + 32);
            match = c == (uint8_t)key[i - at];
        }
        if (match) {
            if (found)
                return false;
            found = true;
            size_t start = colon + 1;
            while (start < end && (raw.data[start] == ' ' || raw.data[start] == '\t'))
                start++;
            size_t trim = end;
            while (trim > start && (raw.data[trim - 1] == ' ' || raw.data[trim - 1] == '\t'))
                trim--;
            *value = (sp5_bytes){raw.data + start, trim - start};
        }
        at = end + 2;
    }
    return true;
}
static bool token_valid(sp5_web *w, sp5_bytes value, uint64_t now)
{
    if (value.size != 64)
        return false;
    for (size_t i = 0; i < SP5_WEB_PAGE_CAPACITY; i++)
        if (w->pages[i].expires_us > now && CRYPTO_memcmp(w->pages[i].token, value.data, 64) == 0)
            return true;
    return false;
}
static bool page(sp5_web *w, uint64_t now, sp5_web_response *r)
{
    if (now > UINT64_MAX - UINT64_C(1800000000) || w->pages_issued == UINT64_MAX)
        return sp5_web_error(r, 503, "clock", "Page lifetime unavailable.", "Restart the host.",
                             w->request_id);
    /* Reuse an expired slot, otherwise replace the oldest issued token. Reloads
     * and extra tabs can never lock management out; a page whose token was
     * replaced receives the stale_token recovery and fetches a new one. */
    size_t slot = 0;
    for (size_t i = 0; i < SP5_WEB_PAGE_CAPACITY; i++) {
        if (w->pages[i].expires_us <= now) {
            slot = i;
            break;
        }
        if (w->pages[i].issued < w->pages[slot].issued)
            slot = i;
    }
    uint8_t random[32];
    if (RAND_bytes(random, sizeof(random)) != 1)
        return sp5_web_error(r, 503, "entropy", "Cannot create page token.", "Retry later.",
                             w->request_id);
    static const char hex[] = "0123456789abcdef";
    for (size_t j = 0; j < sizeof(random); j++) {
        w->pages[slot].token[j * 2] = hex[random[j] >> 4];
        w->pages[slot].token[j * 2 + 1] = hex[random[j] & 15];
    }
    OPENSSL_cleanse(random, sizeof(random));
    w->pages[slot].expires_us = now + UINT64_C(1800000000);
    w->pages[slot].issued = ++w->pages_issued;
    return sp5_json_raw(&r->body, "{\"operation\":\"page\",\"token\":") &&
           sp5_web_text(&r->body, w->pages[slot].token) && sp5_json_raw(&r->body, "}");
}
static bool request_identifier(const sp5_json_field *field, uint64_t *id)
{
    if (!field)
        return false;
    if (field->type == SP5_JSON_UINT) {
        *id = field->number;
        return *id > 0 && *id <= UINT64_C(9007199254740991);
    }
    if (field->type != SP5_JSON_TEXT || field->size == 0 || field->size > 20 ||
        field->text[0] == '0')
        return false;
    *id = 0;
    for (size_t i = 0; i < field->size; ++i) {
        uint8_t c = (uint8_t)field->text[i];
        if (c < '0' || c > '9' || *id > (UINT64_MAX - (c - '0')) / 10)
            return false;
        *id = *id * 10 + (c - '0');
    }
    return *id > 0;
}
/* Contract: 429 carries the remaining cooldown, in words and microseconds. */
static bool cooldown_error(sp5_web *w, uint64_t remaining_us, sp5_web_response *r)
{
    unsigned long long seconds = (unsigned long long)((remaining_us + 999999) / 1000000);
    char message[128], recovery[128];
    int m = snprintf(message, sizeof(message),
                     "Pairing is paused for %llu more seconds after five incorrect PINs.", seconds);
    int n = snprintf(recovery, sizeof(recovery),
                     "Wait %llu seconds, then start pairing again in Moonlight.", seconds);
    if (m < 0 || (size_t)m >= sizeof(message) || n < 0 || (size_t)n >= sizeof(recovery) ||
        !sp5_web_error(r, 429, "pairing_cooldown", message, recovery, w->request_id) ||
        r->body.size < 2)
        return false;
    r->body.size -= 2; /* Reopen the closing "}}" to append the retry time. */
    return sp5_json_raw(&r->body, ",\"retry_after_us\":") && sp5_json_u64(&r->body, remaining_us) &&
           sp5_json_raw(&r->body, "}}");
}
static bool pairing(sp5_web *w, const sp5_request *q, const sp5_json_object *body, uint64_t now,
                    sp5_web_response *r)
{
    if (!w->config.pairing)
        return sp5_web_error(r, 503, "pairing_unavailable", "Pairing service unavailable.",
                             "Retry when pairing is available.", w->request_id);
    sp5_pair_view p = sp5_pairing_read(w->config.pairing, now);
    uint64_t cooldown = p.cooldown_until_us > now ? p.cooldown_until_us - now : 0;
    if (sp5_web_equal(q->method, "GET") && sp5_web_equal(q->target, "/api/v1/pairing")) {
        bool pending = p.request_id && p.state == SP5_PAIR_PENDING;
        /* Decimal strings round-trip random uint64 IDs through browser JSON. */
        bool ok = sp5_json_raw(&r->body, "{\"operation\":\"pairing\",\"cooldown_remaining_us\":") &&
                  sp5_json_u64(&r->body, cooldown) && sp5_json_raw(&r->body, ",\"pending\":") &&
                  sp5_json_raw(&r->body, pending ? "{\"id\":\"" : "null");
        if (pending)
            ok = ok && sp5_json_u64(&r->body, p.request_id) &&
                 sp5_json_raw(&r->body, "\",\"client_name\":") &&
                 sp5_json_string(&r->body, (sp5_bytes){p.client_name, p.client_name_size}) &&
                 sp5_json_raw(&r->body, ",\"expires_in_us\":") &&
                 sp5_json_u64(&r->body, p.expires_us > now ? p.expires_us - now : 0) &&
                 sp5_json_raw(&r->body, ",\"pin_entered\":") &&
                 sp5_json_raw(&r->body, sp5_pair_flow_pin_entered(w->config.pair_flow, p.request_id)
                                            ? "true}"
                                            : "false}");
        /* The most recent ended request stays visible so the page can explain
         * a wrong PIN, an expiry or a completion instead of silently resetting. */
        ok = ok && sp5_json_raw(&r->body, ",\"last\":");
        if (pending || !p.request_id)
            return ok && sp5_json_raw(&r->body, "null}");
        unsigned remaining = cooldown ? 0 : p.failures >= 5 ? 1 : 5 - p.failures;
        return ok && sp5_json_raw(&r->body, "{\"id\":\"") && sp5_json_u64(&r->body, p.request_id) &&
               sp5_json_raw(&r->body, "\",\"state\":") &&
               sp5_web_text(&r->body, p.state == SP5_PAIR_COMPLETED ? "completed"
                                      : p.state == SP5_PAIR_FAILED  ? "failed"
                                                                    : "expired") &&
               sp5_json_raw(&r->body, ",\"client_name\":") &&
               sp5_json_string(&r->body, (sp5_bytes){p.client_name, p.client_name_size}) &&
               sp5_json_raw(&r->body, ",\"failures\":") && sp5_json_u64(&r->body, p.failures) &&
               sp5_json_raw(&r->body, ",\"attempts_before_cooldown\":") &&
               sp5_json_u64(&r->body, remaining) && sp5_json_raw(&r->body, "}}");
    }
    const char *const keys[] = {"request_id", "pin", "device_name"};
    const sp5_json_field *id = sp5_json_get(body, "request_id"), *pin = sp5_json_get(body, "pin"),
                         *name = sp5_json_get(body, "device_name");
    uint64_t request_id = 0;
    if (!sp5_json_keys(body, keys, 3) || !request_identifier(id, &request_id) || !pin ||
        pin->type != SP5_JSON_TEXT || pin->size != 4 ||
        (name && (name->type != SP5_JSON_TEXT ||
                  !sp5_name_valid((sp5_bytes){(const uint8_t *)name->text, name->size}))))
        return sp5_web_error(r, 400, "invalid_pairing",
                             "Enter four PIN digits and a valid optional device name.",
                             "Correct the form and retry.", w->request_id);
    for (size_t i = 0; i < pin->size; i++)
        if (pin->text[i] < '0' || pin->text[i] > '9')
            return sp5_web_error(r, 400, "invalid_pin", "Enter four PIN digits.",
                                 "Correct the PIN.", w->request_id);
    if (cooldown)
        return cooldown_error(w, cooldown, r);
    if (request_id != p.request_id || p.state != SP5_PAIR_PENDING)
        return sp5_web_error(r, 409, "stale_pairing", "This pairing request is no longer pending.",
                             "Start pairing again in Moonlight, then enter its new PIN.",
                             w->request_id);
    if (!w->config.pair_flow)
        return sp5_web_error(r, 503, "pairing_unavailable", "Pairing proof service unavailable.",
                             "Retry when pairing is available.", w->request_id);
    if (sp5_pair_flow_pin_entered(w->config.pair_flow, request_id))
        return sp5_web_error(
            r, 409, "pin_already_entered", "A PIN was already entered for this pairing request.",
            "Wait for Moonlight to finish; this page shows the result.", w->request_id);
    sp5_result result = sp5_pair_flow_submit_pin(
        w->config.pair_flow, request_id, now, (sp5_bytes){(const uint8_t *)pin->text, pin->size},
        name ? (sp5_bytes){(const uint8_t *)name->text, name->size} : (sp5_bytes){0});
    if (result.status != SP5_OK)
        return sp5_web_error(r, result.reason == SP5_REASON_WOULD_BLOCK ? 409 : 400,
                             "pairing_failed", "PIN submission failed.",
                             "Check the PIN and restart pairing if necessary.", w->request_id);
    return sp5_json_raw(&r->body, "{\"operation\":\"pairing_pin\",\"state\":\"proof_pending\"}");
}
sp5_result sp5_web_dispatch(sp5_web *w, const char *peer, bool on_link, sp5_bytes raw, uint64_t now,
                            sp5_web_response *r)
{
    if (!w || !r || !r->body.data)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    r->body.size = 0;
    r->status = 200;
    r->content_type = "application/json";
    r->stopping = false;
    r->stream = NULL;
    r->stream_context = NULL;
    r->download_filename = NULL;
    if (w->request_id == UINT64_MAX)
        return sp5_failure(SP5_REASON_CAPACITY);
    r->request_id = ++w->request_id;
    bool ok = true;
    sp5_request q = {0};
    sp5_json_object body = {0};
    if (!sp5_peer_permitted(peer, on_link))
        ok = sp5_web_error(r, 403, "peer_denied", "Private network access required.",
                           "Connect from a permitted private network.", w->request_id);
    else if (now < w->now_us)
        ok = sp5_web_error(r, 503, "clock", "Management clock moved backwards.",
                           "Restart the host.", w->request_id);
    else if (sp5_http_parse(raw, 8192, &q).status != SP5_OK || q.body.size > SP5_WEB_BODY_LIMIT)
        ok = sp5_web_error(r, 400, "invalid_request", "Malformed or oversized request.",
                           "Send a bounded HTTP request.", w->request_id);
    else {
        w->now_us = now;
        sp5_bytes host = {0}, origin = {0}, token = {0}, type = {0}, site = {0}, dest = {0};
        bool headers = header(raw, "host", &host) && header(raw, "origin", &origin) &&
                       header(raw, "x-sp5-token", &token) && header(raw, "content-type", &type) &&
                       header(raw, "sec-fetch-site", &site) && header(raw, "sec-fetch-dest", &dest);
        bool get = sp5_web_equal(q.method, "GET");
        bool api = q.target.size >= 5 && memcmp(q.target.data, "/api/", 5) == 0;
        const sp5_web_asset *asset = get && !api ? sp5_web_asset_find(q.target) : NULL;
        if (!headers || !sp5_web_equal(host, w->authority) ||
            (origin.data && !sp5_web_equal(origin, w->origin)) || sp5_web_equal(site, "cross-site"))
            ok = sp5_web_error(r, 403, "origin_denied", "Host or Origin is not the served address.",
                               "Open the host address directly.", w->request_id);
        else if (!get && !token_valid(w, token, now))
            ok = sp5_web_error(r, 403, "stale_token", "This management page has expired.",
                               "Reload the page and retry.", w->request_id);
        else if (!get &&
                 (!sp5_web_equal(type, "application/json") || !sp5_json_parse(q.body, &body)))
            ok = sp5_web_error(r, 400, "invalid_json",
                               "A bounded JSON object with unique fields is required.",
                               "Correct the request and retry.", w->request_id);
        else if (get && q.body.size)
            ok = sp5_web_error(r, 400, "unexpected_body", "GET requests cannot contain a body.",
                               "Remove the request body.", w->request_id);
        else if (!get && w->config.host->state == SP5_HOST_STOPPING &&
                 !sp5_web_equal(q.target, "/api/v1/host/stop"))
            ok = sp5_web_error(r, 409, "host_stopping", "Host is stopping.", "Wait for shutdown.",
                               w->request_id);
        else if (get && sp5_web_equal(q.target, "/api/v1/page") && dest.data &&
                 !sp5_web_equal(dest, "empty"))
            /* Tokens go only to the app's fetch(), never to embedded loads. */
            ok = sp5_web_error(r, 403, "origin_denied", "Page tokens are issued only to the app.",
                               "Open the host address directly.", w->request_id);
        else if (get && sp5_web_equal(q.target, "/api/v1/page"))
            ok = page(w, now, r);
        else if (get && sp5_web_equal(q.target, "/api/v1/status"))
            ok = sp5_web_status(w, now, r);
        else if ((get && sp5_web_equal(q.target, "/api/v1/pairing")) ||
                 (sp5_web_equal(q.method, "POST") &&
                  sp5_web_equal(q.target, "/api/v1/pairing/pin")))
            ok = pairing(w, &q, &body, now, r);
        else if ((get && sp5_web_equal(q.target, "/api/v1/devices")) ||
                 ((sp5_web_equal(q.method, "PATCH") || sp5_web_equal(q.method, "DELETE")) &&
                  q.target.size > 16 && memcmp(q.target.data, "/api/v1/devices/", 16) == 0))
            ok = sp5_web_devices(w, &q, &body, r);
        else if (sp5_web_equal(q.method, "PATCH") && sp5_web_equal(q.target, "/api/v1/settings"))
            ok = sp5_web_settings(w, &body, now, r);
        else if (sp5_web_equal(q.method, "POST") && sp5_web_equal(q.target, "/api/v1/host/stop"))
            ok = sp5_web_stop(w, &body, r);
        else if (w->config.extensions.route &&
                 w->config.extensions.route(w->config.extensions.context, &q, now, r))
            ok = true;
        else if (asset) {
            r->content_type = asset->content_type;
            ok =
                sp5_buffer_append(&r->body, (sp5_bytes){asset->data, asset->size}).status == SP5_OK;
        } else
            ok = sp5_web_error(r, 404, "not_found", "Operation not found.",
                               "Check the URL and method.", w->request_id);
    }
    return ok ? sp5_success() : sp5_failure(SP5_REASON_CAPACITY);
}
