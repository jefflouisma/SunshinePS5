/* T077 web pairing contract: the real sp5_web routes, pair flow, registry and
 * store driven in process. The client half of the Gen7 exchange is SYNTHETIC
 * (tests/fakes/pairing_client.c); no official client, console or reference
 * host result is claimed. Clocks are synthetic monotonic microseconds. */
#include "../fakes/pairing_client.h"
#include "../fakes/platform.h"
#include "web/assets.h"
#include "web/diagnostics.h"
#include "web/metrics.h"
#include "web/server.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT(x) #x
#define LINE(x) TEXT(x)
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs(__FILE__ ":" LINE(__LINE__) ": " #c "\n", stderr) == EOF)                    \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

enum { RESPONSE_BYTES = SP5_WEB_RESPONSE_LIMIT + 1, RAW_BYTES = 8192 };
static const char authority[] = "console.local:8080";
static const char peer[] = "192.168.80.4";
static const char client_pin[] = "4821";
static uint8_t server_identity[SP5_IDENTITY_CAPACITY], client_identity[SP5_IDENTITY_CAPACITY];
static size_t server_identity_size, client_identity_size;

static sp5_bytes text(const char *s)
{
    return (sp5_bytes){(const uint8_t *)s, strlen(s)};
}
typedef struct {
    uint8_t bytes[SP5_STORE_FILE_COUNT][40000];
    size_t sizes[SP5_STORE_FILE_COUNT];
} disk;
static sp5_result read_file(void *p, sp5_store_file f, sp5_buffer *out)
{
    disk *d = p;
    return d->sizes[f] ? sp5_buffer_append(out, (sp5_bytes){d->bytes[f], d->sizes[f]})
                       : sp5_failure(SP5_REASON_WOULD_BLOCK);
}
static sp5_result write_file(void *p, sp5_store_file f, sp5_bytes bytes)
{
    disk *d = p;
    if (bytes.size > sizeof(d->bytes[f]))
        return sp5_failure(SP5_REASON_CAPACITY);
    memcpy(d->bytes[f], bytes.data, bytes.size);
    d->sizes[f] = bytes.size;
    return sp5_success();
}
static sp5_result identity(void *p, sp5_buffer *out)
{
    (void)p;
    return sp5_buffer_append(out, (sp5_bytes){server_identity, server_identity_size});
}
typedef struct {
    unsigned count;
    char last[320];
} notices;
static sp5_result notify(void *context, const char *message, size_t size)
{
    notices *n = context;
    if (size >= sizeof(n->last))
        return sp5_failure(SP5_REASON_CAPACITY);
    memcpy(n->last, message, size);
    n->last[size] = 0;
    ++n->count;
    return sp5_success();
}
typedef struct {
    disk storage;
    notices notified;
    sp5_store *store;
    sp5_host host;
    sp5_authority *authority;
    sp5_session *session;
    sp5_pairing *pairing;
    sp5_pair_flow *flow;
    sp5_web *web;
    sp5_web_extensions extensions;
    char token[65];
    uint64_t now;
    uint8_t response[RESPONSE_BYTES];
    sp5_web_response result;
} fixture;

static sp5_bytes certificate_span(sp5_bytes value)
{
    static const char marker[] = "-----BEGIN CERTIFICATE-----";
    for (size_t i = 0; i + sizeof(marker) - 1 <= value.size; ++i)
        if (memcmp(value.data + i, marker, sizeof(marker) - 1) == 0)
            return (sp5_bytes){value.data + i, value.size - i};
    return (sp5_bytes){0};
}
static int send_raw(fixture *f, const char *from, const char *raw)
{
    f->result = (sp5_web_response){.body = {f->response, sizeof(f->response) - 1, 0}};
    CHECK(sp5_web_dispatch(f->web, from, false, text(raw), f->now, &f->result).status == SP5_OK);
    f->response[f->result.body.size] = 0;
    return 0;
}
/* headers excludes Content-Length, which is always computed from body. */
typedef struct {
    const char *peer, *method, *path, *headers, *body;
} wire_request;
static int custom(fixture *f, wire_request q)
{
    char raw[RAW_BYTES];
    int n = snprintf(raw, sizeof(raw), "%s %s HTTP/1.1\r\n%sContent-Length: %zu\r\n\r\n%s",
                     q.method, q.path, q.headers, strlen(q.body), q.body);
    CHECK(n > 0 && (size_t)n < sizeof(raw));
    return send_raw(f, q.peer, raw);
}
typedef struct {
    const char *method, *path, *body;
} api_call;
static int api_token(fixture *f, api_call call, const char *token)
{
    char headers[512];
    int n = snprintf(headers, sizeof(headers),
                     "Host: %s\r\nOrigin: http://%s\r\nContent-Type: application/json\r\n"
                     "X-SP5-Token: %s\r\n",
                     authority, authority, token);
    CHECK(n > 0 && (size_t)n < sizeof(headers));
    return custom(f, (wire_request){peer, call.method, call.path, headers, call.body});
}
static int api(fixture *f, const char *method, const char *path, const char *body)
{
    return api_token(f, (api_call){method, path, body}, f->token);
}
static bool has(const fixture *f, const char *needle)
{
    return strstr((const char *)f->response, needle) != NULL;
}
static int token(fixture *f, char out[65])
{
    CHECK(api(f, "GET", "/api/v1/page", "") == 0 && f->result.status == 200);
    const char *start = strstr((char *)f->response, "\"token\":\"");
    CHECK(start != NULL && strlen(start + 9) >= 65 && start[9 + 64] == '"');
    memcpy(out, start + 9, 64);
    out[64] = 0;
    return 0;
}
static int keys(void)
{
    if (server_identity_size)
        return 0;
    sp5_buffer server = {server_identity, sizeof(server_identity), 0};
    sp5_buffer client = {client_identity, sizeof(client_identity), 0};
    CHECK(sp5_fake_pair_identity(&server).status == SP5_OK);
    CHECK(sp5_fake_pair_identity(&client).status == SP5_OK);
    server_identity_size = server.size;
    client_identity_size = client.size;
    return 0;
}
static int open_fixture(fixture *f, uint64_t now)
{
    CHECK(keys() == 0);
    f->now = now;
    sp5_storage_ops ops = {&f->storage, read_file, write_file, identity};
    f->store = sp5_store_create(&ops);
    CHECK(f->store != NULL && sp5_store_load(f->store).status == SP5_OK);
    CHECK(sp5_host_init(&f->host, "portable-test", "13.60", "192.168.80.1").status == SP5_OK);
    CHECK(sp5_host_refresh(&f->host, f->store).status == SP5_OK);
    CHECK(sp5_host_transition(&f->host, SP5_HOST_RUNNING).status == SP5_OK);
    sp5_devices *devices = sp5_store_devices(f->store);
    f->authority = sp5_authority_create(devices);
    f->session = f->authority == NULL ? NULL : sp5_session_create(f->authority);
    f->pairing = sp5_pairing_create();
    CHECK(f->session != NULL && f->pairing != NULL);
    sp5_bytes stored = sp5_store_identity(f->store);
    sp5_bytes certificate = certificate_span(stored);
    CHECK(certificate.size != 0);
    sp5_pair_flow_config pairing = {
        .state = f->pairing,
        .devices = devices,
        .certificate_pem = certificate,
        .private_key_pem = {stored.data, stored.size - certificate.size},
        .system = {.context = &f->notified, .notify = notify},
        .pin_url = "http://console.local:8080/"};
    f->flow = sp5_pair_flow_create(&pairing);
    CHECK(f->flow != NULL);
    sp5_web_config config = {.store = f->store,
                             .host = &f->host,
                             .session = f->session,
                             .pairing = f->pairing,
                             .pair_flow = f->flow,
                             .authority = authority,
                             .extensions = f->extensions};
    f->web = sp5_web_create(&config);
    CHECK(f->web != NULL);
    return token(f, f->token);
}
static void close_fixture(fixture *f)
{
    sp5_web_destroy(f->web);
    sp5_pair_flow_destroy(f->flow);
    sp5_pairing_destroy(f->pairing);
    sp5_session_destroy(f->session);
    sp5_authority_destroy(f->authority);
    sp5_store_destroy(f->store);
}
/* Rejected submissions leave the pending request, its PIN slot, the failure
 * counter and the registry exactly as they were. */
typedef struct {
    uint64_t id;
    unsigned failures;
    size_t devices;
} pending_state;
static int unchanged(fixture *f, pending_state expected)
{
    sp5_pair_view view = sp5_pairing_read(f->pairing, f->now);
    CHECK(view.state == SP5_PAIR_PENDING && view.request_id == expected.id &&
          view.failures == expected.failures);
    CHECK(!sp5_pair_flow_pin_entered(f->flow, expected.id));
    CHECK(sp5_devices_count(sp5_store_devices(f->store)) == expected.devices);
    return 0;
}
static int begin(fixture *f, sp5_fake_pair_client *client, const char *name, uint64_t *id)
{
    unsigned code = 0;
    bool deferred = false;
    CHECK(sp5_fake_pair_client_begin(client, f->flow, f->pairing, text(name), f->now, &code,
                                     &deferred)
              .status == SP5_OK);
    if ((code != 200 || !deferred) && fputs("begin: unexpected HTTP code\n", stderr) == EOF)
        return 2;
    CHECK(code == 200 && deferred && client->request_id != 0);
    *id = client->request_id;
    return 0;
}
static int submit(fixture *f, uint64_t id, const char *pin, const char *extra)
{
    char body[512];
    int n = snprintf(body, sizeof(body), "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"%s\"%s}", id,
                     pin, extra);
    CHECK(n > 0 && (size_t)n < sizeof(body));
    return api(f, "POST", "/api/v1/pairing/pin", body);
}
static int prove(fixture *f, sp5_fake_pair_client *client, bool expected)
{
    bool paired = !expected;
    CHECK(sp5_fake_pair_client_prove(client, f->flow, text(client_pin), f->now,
                                     (sp5_optional_u64){true, UINT64_C(1700000000000000)}, &paired)
              .status == SP5_OK);
    CHECK(paired == expected);
    return 0;
}
static int last(fixture *f, uint64_t id, const char *state, unsigned failures, unsigned remaining)
{
    char expected[256];
    int n =
        snprintf(expected, sizeof(expected),
                 "\"last\":{\"id\":\"%" PRIu64 "\",\"state\":\"%s\",\"client_name\":", id, state);
    CHECK(n > 0 && (size_t)n < sizeof(expected));
    CHECK(api(f, "GET", "/api/v1/pairing", "") == 0 && f->result.status == 200);
    CHECK(has(f, "\"pending\":null") && has(f, expected));
    n = snprintf(expected, sizeof(expected), "\"failures\":%u,\"attempts_before_cooldown\":%u}",
                 failures, remaining);
    CHECK(n > 0 && (size_t)n < sizeof(expected) && has(f, expected));
    return 0;
}

static int boundaries(fixture *f, uint64_t id)
{
    char good[512], body[128];
    int n = snprintf(good, sizeof(good),
                     "Host: %s\r\nOrigin: http://%s\r\nContent-Type: application/json\r\n"
                     "X-SP5-Token: %s\r\n",
                     authority, authority, f->token);
    CHECK(n > 0 && (size_t)n < sizeof(good));
    n = snprintf(body, sizeof(body), "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"%s\"}", id,
                 client_pin);
    CHECK(n > 0 && (size_t)n < sizeof(body));
    char hostile[6][512];
    const char *variants[6][2] = {
        {"Host: evil.test\r\n", "evil Host"},
        {"Host: console.local:8080\r\nHost: console.local:8080\r\n", "duplicate Host"},
        {"Host: console.local:8080\r\nOrigin: https://evil.test\r\n", "foreign Origin"},
        {"Host: console.local:8080\r\nOrigin: null\r\n", "opaque Origin"},
        {"Host: console.local:8080\r\nSec-Fetch-Site: cross-site\r\n", "cross-site fetch"},
        {"Host: console.local:8080\r\nOrigin: http://console.local:8080\r\nOrigin: "
         "http://console.local:8080\r\n",
         "duplicate Origin"}};
    for (size_t i = 0; i < 6; ++i) {
        n = snprintf(hostile[i], sizeof(hostile[i]),
                     "%sContent-Type: application/json\r\nX-SP5-Token: %s\r\n", variants[i][0],
                     f->token);
        CHECK(n > 0 && (size_t)n < sizeof(hostile[i]));
    }
    const char *peers[] = {"8.8.8.8", "2001:db8::1", "::ffff:8.8.8.8", "fe80::1"};
    for (size_t i = 0; i < sizeof(peers) / sizeof(peers[0]); ++i) {
        CHECK(custom(f, (wire_request){peers[i], "GET", "/api/v1/pairing", good, ""}) == 0);
        CHECK(f->result.status == 403 && has(f, "peer_denied") && !has(f, "client_name"));
        CHECK(custom(f, (wire_request){peers[i], "POST", "/api/v1/pairing/pin", good, body}) == 0);
        CHECK(f->result.status == 403 && unchanged(f, (pending_state){id, 0, 0}) == 0);
    }
    for (size_t i = 0; i < 6; ++i) {
        CHECK(custom(f, (wire_request){peer, "GET", "/api/v1/pairing", hostile[i], ""}) == 0);
        CHECK(f->result.status == 403 && has(f, "origin_denied") && !has(f, "client_name"));
        CHECK(custom(f, (wire_request){peer, "POST", "/api/v1/pairing/pin", hostile[i], body}) ==
              0);
        CHECK(f->result.status == 403 && unchanged(f, (pending_state){id, 0, 0}) == 0);
    }
    const char *tokens[] = {"", "stale",
                            "0000000000000000000000000000000000000000000000000000000000000000"};
    for (size_t i = 0; i < 3; ++i) {
        CHECK(api_token(f, (api_call){"POST", "/api/v1/pairing/pin", body}, tokens[i]) == 0);
        CHECK(f->result.status == 403 && has(f, "stale_token") &&
              unchanged(f, (pending_state){id, 0, 0}) == 0);
    }
    char missing[256];
    n = snprintf(missing, sizeof(missing),
                 "Host: %s\r\nOrigin: http://%s\r\nContent-Type: application/json\r\n", authority,
                 authority);
    CHECK(n > 0 && (size_t)n < sizeof(missing));
    CHECK(custom(f, (wire_request){peer, "POST", "/api/v1/pairing/pin", missing, body}) == 0);
    CHECK(f->result.status == 403 && unchanged(f, (pending_state){id, 0, 0}) == 0);
    char plain[512];
    n = snprintf(plain, sizeof(plain),
                 "Host: %s\r\nContent-Type: text/plain\r\nX-SP5-Token: %s\r\n", authority,
                 f->token);
    CHECK(n > 0 && (size_t)n < sizeof(plain));
    CHECK(custom(f, (wire_request){peer, "POST", "/api/v1/pairing/pin", plain, body}) == 0);
    CHECK(f->result.status == 400 && has(f, "invalid_json") &&
          unchanged(f, (pending_state){id, 0, 0}) == 0);
    /* No Origin (some browsers omit it): the page token alone still gates. */
    n = snprintf(plain, sizeof(plain),
                 "Host: %s\r\nContent-Type: application/json\r\nX-SP5-Token: stale\r\n", authority);
    CHECK(n > 0 && (size_t)n < sizeof(plain));
    CHECK(custom(f, (wire_request){peer, "POST", "/api/v1/pairing/pin", plain, body}) == 0);
    CHECK(f->result.status == 403 && unchanged(f, (pending_state){id, 0, 0}) == 0);
    return 0;
}

static int invalid_submissions(fixture *f, uint64_t id)
{
    char cases[22][256];
    unsigned status[22];
    size_t count = 0;
#define CASE(code, ...)                                                                            \
    do {                                                                                           \
        int written = snprintf(cases[count], sizeof(cases[count]), __VA_ARGS__);                   \
        CHECK(written > 0 && (size_t)written < sizeof(cases[count]));                              \
        status[count++] = (code);                                                                  \
    } while (0)
    CASE(409, "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"4821\"}", id + 1);
    CASE(400, "{\"request_id\":\"0\",\"pin\":\"4821\"}");
    CASE(400, "{\"request_id\":\"0%" PRIu64 "\",\"pin\":\"4821\"}", id);
    CASE(400, "{\"request_id\":9007199254740993,\"pin\":\"4821\"}");
    CASE(400, "{\"request_id\":\"18446744073709551616\",\"pin\":\"4821\"}");
    CASE(400, "{\"request_id\":\"%" PRIu64 "x\",\"pin\":\"4821\"}", id);
    CASE(400, "{\"request_id\":\"-1\",\"pin\":\"4821\"}");
    CASE(400, "{\"pin\":\"4821\"}");
    CASE(400, "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"482\"}", id);
    CASE(400, "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"48211\"}", id);
    CASE(400, "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"48a1\"}", id);
    CASE(400, "{\"request_id\":\"%" PRIu64 "\",\"pin\":4821}", id);
    CASE(400, "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"4821\",\"extra\":true}", id);
    CASE(400, "{\"request_id\":\"%" PRIu64 "\",\"request_id\":\"%" PRIu64 "\",\"pin\":\"4821\"}",
         id, id);
    CASE(400, "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"4821\",\"device_name\":\"a\\u0001b\"}",
         id);
    CASE(400, "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"4821\",\"device_name\":\"a\\u0085b\"}",
         id);
    CASE(400, "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"4821\",\"device_name\":\"\\ud800\"}", id);
    CASE(400, "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"4821\",\"device_name\":\"\"}", id);
    CASE(400, "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"4821\",\"device_name\":7}", id);
    CASE(400, "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"4821\",\"device_name\":\"%s\"}", id,
         "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijkl");
    CASE(400, "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"4821\"", id);
    CASE(400, "[\"%" PRIu64 "\",\"4821\"]", id);
#undef CASE
    for (size_t i = 0; i < count; ++i) {
        CHECK(api(f, "POST", "/api/v1/pairing/pin", cases[i]) == 0);
        if (f->result.status != status[i] &&
            (fputs(cases[i], stderr) == EOF || fputs((const char *)f->response, stderr) == EOF))
            return 2;
        CHECK(f->result.status == status[i] && has(f, "\"recovery\":"));
        CHECK(unchanged(f, (pending_state){id, 0, 0}) == 0);
    }
    return 0;
}

/* Static storage: each case runs once, and an early CHECK return leaks nothing. */
static fixture fixture_storage;
static sp5_fake_pair_client client_storage;
static uint8_t source_storage[SP5_WEB_RESPONSE_LIMIT + 1];
static fixture *fresh_fixture(void)
{
    memset(&fixture_storage, 0, sizeof(fixture_storage));
    return &fixture_storage;
}
static sp5_fake_pair_client *fresh_client(void)
{
    memset(&client_storage, 0, sizeof(client_storage));
    return &client_storage;
}

static int journey(void)
{
    fixture *f = fresh_fixture();
    sp5_fake_pair_client *client = fresh_client();
    CHECK(open_fixture(f, UINT64_C(10000000)) == 0);
    CHECK(sp5_fake_pair_client_open(client, (sp5_bytes){client_identity, client_identity_size})
              .status == SP5_OK);
    CHECK(api(f, "GET", "/api/v1/pairing", "") == 0 && f->result.status == 200);
    CHECK(has(f, "\"cooldown_remaining_us\":0,\"pending\":null,\"last\":null}"));
    /* Untrusted Moonlight metadata: control bytes never reach the pairing view;
     * markup, quote, backslash and invalid UTF-8 arrive escaped in valid JSON. */
    unsigned refused = 0;
    bool waiting = true;
    CHECK(sp5_fake_pair_client_begin(client, f->flow, f->pairing, text("bad\x01name"), f->now,
                                     &refused, &waiting)
              .status == SP5_OK);
    CHECK(refused == 400 && !waiting && sp5_pairing_read(f->pairing, f->now).request_id == 0);
    uint64_t a = 0;
    CHECK(begin(f, client, "<img src=x onerror=alert(1)>\"q\\\xff", &a) == 0);
    char expected[256];
    int n = snprintf(expected, sizeof(expected), "\"pending\":{\"id\":\"%" PRIu64 "\"", a);
    CHECK(n > 0 && (size_t)n < sizeof(expected));
    CHECK(api(f, "GET", "/api/v1/pairing", "") == 0 && f->result.status == 200);
    CHECK(has(f, expected) && has(f, "\"pin_entered\":false") && has(f, "\"last\":null"));
    CHECK(has(f, "\"client_name\":\"<img src=x onerror=alert(1)>\\\"q\\\\\\u00ff\""));
    CHECK(has(f, "\"expires_in_us\":300000000"));
    CHECK(f->notified.count == 1 && strstr(f->notified.last, "Enter its PIN") != NULL);
    CHECK(boundaries(f, a) == 0);
    CHECK(invalid_submissions(f, a) == 0);
    /* Correct PIN through the web route, then the synthetic client's proof. */
    f->now += 1000000;
    CHECK(submit(f, a, client_pin, ",\"device_name\":\"Den <phone>\"") == 0);
    CHECK(f->result.status == 200 &&
          has(f, "{\"operation\":\"pairing_pin\",\"state\":\"proof_pending\"}"));
    CHECK(api(f, "GET", "/api/v1/pairing", "") == 0 && has(f, "\"pin_entered\":true"));
    CHECK(submit(f, a, client_pin, "") == 0 && f->result.status == 409);
    CHECK(has(f, "pin_already_entered") && sp5_pair_flow_pin_entered(f->flow, a));
    uint64_t submitted = f->now;
    f->now += 500000;
    CHECK(prove(f, client, true) == 0);
    /* Completion is visible on the very next poll, well inside three seconds. */
    CHECK(f->now - submitted < UINT64_C(3000000));
    CHECK(last(f, a, "completed", 0, 5) == 0);
    CHECK(f->notified.count == 2 && strstr(f->notified.last, "Den <phone>") != NULL);
    CHECK(api(f, "GET", "/api/v1/devices", "") == 0 && f->result.status == 200);
    CHECK(has(f, "\"name\":\"Den <phone>\"") && !has(f, "CERTIFICATE"));
    /* Paired status persists: a new store over the same durable files. */
    sp5_storage_ops ops = {&f->storage, read_file, write_file, identity};
    sp5_store *reloaded = sp5_store_create(&ops);
    CHECK(reloaded != NULL && sp5_store_load(reloaded).status == SP5_OK);
    CHECK(sp5_store_authorized(reloaded,
                               (sp5_bytes){client->certificate_pem, client->certificate_size}));
    CHECK(sp5_store_read(reloaded).paired_count == 1);
    sp5_store_destroy(reloaded);
    /* A form still holding request A cannot approve its successor B. */
    uint64_t b = 0;
    CHECK(begin(f, client, "Second", &b) == 0 && b != a);
    CHECK(submit(f, a, client_pin, "") == 0 && f->result.status == 409);
    CHECK(has(f, "stale_pairing") && unchanged(f, (pending_state){b, 0, 1}) == 0);
    /* Wrong PIN: the client still completes its side and the host refuses. */
    CHECK(submit(f, b, "0000", "") == 0 && f->result.status == 200);
    CHECK(prove(f, client, false) == 0);
    CHECK(last(f, b, "failed", 1, 4) == 0);
    CHECK(sp5_devices_count(sp5_store_devices(f->store)) == 1);
    for (unsigned failure = 2; failure <= 5; ++failure) {
        f->now += 1000000;
        uint64_t next = 0;
        CHECK(begin(f, client, "Retry", &next) == 0);
        CHECK(submit(f, next, "0000", "") == 0 && f->result.status == 200);
        CHECK(prove(f, client, false) == 0);
        CHECK(last(f, next, "failed", failure, failure == 5 ? 0 : 5 - failure) == 0);
        b = next;
    }
    /* Cooldown: reported with its remaining time, refused for 60 seconds. */
    f->now += 1000000;
    CHECK(api(f, "GET", "/api/v1/pairing", "") == 0 &&
          has(f, "\"cooldown_remaining_us\":59000000"));
    CHECK(submit(f, b, client_pin, "") == 0 && f->result.status == 429);
    CHECK(has(f, "\"code\":\"pairing_cooldown\"") && has(f, "59 more seconds") &&
          has(f, "\"retry_after_us\":59000000") && has(f, "\"request_id\":"));
    unsigned code = 0;
    bool deferred = true;
    CHECK(sp5_fake_pair_client_begin(client, f->flow, f->pairing, text("Blocked"), f->now, &code,
                                     &deferred)
              .status == SP5_OK);
    CHECK(code == 429 && !deferred);
    /* Expiry after the cooldown ends. */
    f->now += 60000000;
    uint64_t g = 0;
    CHECK(begin(f, client, "Late", &g) == 0);
    f->now += 300000000;
    CHECK(last(f, g, "expired", 5, 1) == 0);
    CHECK(submit(f, g, client_pin, "") == 0 && f->result.status == 409);
    sp5_fake_pair_client_close(client);
    close_fixture(f);
    return 0;
}

static int long_names(void)
{
    fixture *f = fresh_fixture();
    sp5_fake_pair_client *client = fresh_client();
    CHECK(open_fixture(f, 1) == 0);
    CHECK(sp5_fake_pair_client_open(client, (sp5_bytes){client_identity, client_identity_size})
              .status == SP5_OK);
    char name[258];
    memset(name, 'W', 257);
    name[257] = 0;
    unsigned code = 0;
    bool deferred = true;
    CHECK(sp5_fake_pair_client_begin(client, f->flow, f->pairing, text(name), f->now, &code,
                                     &deferred)
              .status == SP5_OK);
    CHECK(code == 400 && !deferred);
    name[256] = 0;
    uint64_t id = 0;
    CHECK(begin(f, client, name, &id) == 0);
    char expected[300];
    int n = snprintf(expected, sizeof(expected), "\"client_name\":\"%s\",", name);
    CHECK(n > 0 && (size_t)n < sizeof(expected));
    CHECK(api(f, "GET", "/api/v1/pairing", "") == 0 && has(f, expected));
    sp5_fake_pair_client_close(client);
    close_fixture(f);
    return 0;
}

/* Page tokens: bounded slots, never a lockout; replaced tokens are stale. */
static int tokens(void)
{
    fixture *f = fresh_fixture();
    CHECK(open_fixture(f, 5) == 0);
    char issued[40][65];
    for (size_t i = 0; i < 40; ++i)
        CHECK(token(f, issued[i]) == 0);
    for (size_t i = 0; i < 40; ++i) {
        CHECK(api_token(f, (api_call){"PATCH", "/api/v1/settings", "{\"host_name\":\"Den\"}"},
                        issued[i]) == 0);
        CHECK(f->result.status == (i < 24 ? 403U : 200U));
        CHECK(i >= 24 || (has(f, "stale_token") && has(f, "Reload the page")));
    }
    CHECK(api(f, "PATCH", "/api/v1/settings", "{\"host_name\":\"Den\"}") == 0 &&
          f->result.status == 403);
    char headers[256];
    const char *destinations[] = {"image", "document", "script", "empty"};
    for (size_t i = 0; i < 4; ++i) {
        int n = snprintf(headers, sizeof(headers), "Host: %s\r\nSec-Fetch-Dest: %s\r\n", authority,
                         destinations[i]);
        CHECK(n > 0 && (size_t)n < sizeof(headers));
        CHECK(custom(f, (wire_request){peer, "GET", "/api/v1/page", headers, ""}) == 0);
        CHECK(f->result.status == (i < 3 ? 403U : 200U));
    }
    /* Thirty-minute lifetime, then a new page token works. */
    f->now += UINT64_C(1800000000);
    CHECK(api_token(f, (api_call){"PATCH", "/api/v1/settings", "{\"host_name\":\"Late\"}"},
                    issued[39]) == 0);
    CHECK(f->result.status == 403 && strcmp(sp5_store_settings(f->store).host_name, "Den") == 0);
    CHECK(token(f, f->token) == 0);
    CHECK(api(f, "PATCH", "/api/v1/settings", "{\"host_name\":\"Late\"}") == 0 &&
          f->result.status == 200);
    close_fixture(f);
    return 0;
}

/* An expired page token cannot approve a pending request created later. */
static int expired_token(void)
{
    fixture *f = fresh_fixture();
    sp5_fake_pair_client *client = fresh_client();
    CHECK(open_fixture(f, 1000000) == 0);
    CHECK(sp5_fake_pair_client_open(client, (sp5_bytes){client_identity, client_identity_size})
              .status == SP5_OK);
    f->now += UINT64_C(1799000000);
    uint64_t id = 0;
    CHECK(begin(f, client, "Phone", &id) == 0);
    f->now += UINT64_C(1000000);
    CHECK(submit(f, id, client_pin, "") == 0 && f->result.status == 403);
    CHECK(has(f, "stale_token") && unchanged(f, (pending_state){id, 0, 0}) == 0);
    sp5_fake_pair_client_close(client);
    close_fixture(f);
    return 0;
}

static int read_source(const char *name, uint8_t *data, size_t capacity, size_t *size)
{
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", SP5_WEBAPP_DIR, name);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    *size = fread(data, 1, capacity, file);
    bool complete = feof(file) != 0 && ferror(file) == 0;
    CHECK(fclose(file) == 0 && complete);
    return 0;
}

static int assets(void)
{
    fixture *f = fresh_fixture();
    uint8_t *source = source_storage;
    CHECK(open_fixture(f, 1) == 0);
    const char *names[] = {"index.html", "app.js",    "status.js",  "devices.js",    "settings.js",
                           "stop.js",    "style.css", "metrics.js", "diagnostics.js"};
    CHECK(sp5_web_asset_count == sizeof(names) / sizeof(names[0]));
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char path[64];
        int n = snprintf(path, sizeof(path), "/%s", names[i]);
        CHECK(n > 0 && (size_t)n < sizeof(path));
        size_t size = 0;
        CHECK(read_source(names[i], source, SP5_WEB_RESPONSE_LIMIT + 1, &size) == 0);
        CHECK(api(f, "GET", path, "") == 0 && f->result.status == 200);
        CHECK(f->result.body.size == size && memcmp(f->response, source, size) == 0);
        const char *type = strstr(names[i], ".html")  ? "text/html; charset=utf-8"
                           : strstr(names[i], ".css") ? "text/css; charset=utf-8"
                                                      : "text/javascript; charset=utf-8";
        CHECK(strcmp(f->result.content_type, type) == 0);
        /* Offline: no asset can name an external origin. */
        CHECK(!has(f, "http://") && !has(f, "https://"));
    }
    CHECK(api(f, "GET", "/", "") == 0 && f->result.status == 200 && has(f, "<!doctype html>"));
    CHECK(has(f, "src=\"/app.js\"") && has(f, "href=\"/style.css\""));
    const char *misses[] = {"/api/v1/index.html", "/api/app.js", "/missing.js", "/index.html?x=1",
                            "/../webapp/app.js",  "//app.js",    "/APP.JS",     "/app.js/"};
    for (size_t i = 0; i < sizeof(misses) / sizeof(misses[0]); ++i) {
        CHECK(api(f, "GET", misses[i], "") == 0 && f->result.status == 404);
        CHECK(strcmp(f->result.content_type, "application/json") == 0 && has(f, "not_found"));
    }
    CHECK(api(f, "POST", "/app.js", "{}") == 0 && f->result.status == 404);
    CHECK(custom(f, (wire_request){"8.8.8.8", "GET", "/app.js", "Host: console.local:8080\r\n",
                                   ""}) == 0);
    CHECK(f->result.status == 403 && !has(f, "import"));
    CHECK(custom(f, (wire_request){peer, "GET", "/", "Host: evil.test\r\n", ""}) == 0);
    CHECK(f->result.status == 403 && !has(f, "doctype"));
    CHECK(custom(f, (wire_request){peer, "GET", "/app.js",
                                   "Host: console.local:8080\r\nSec-Fetch-Site: cross-site\r\n",
                                   ""}) == 0);
    CHECK(f->result.status == 403 && !has(f, "import"));
    /* Shared response head policy. */
    uint8_t head[1024];
    sp5_buffer out = {head, sizeof(head) - 1, 0};
    sp5_web_response json = {.status = 429, .content_type = "application/json"};
    json.body.size = 17;
    CHECK(sp5_web_format_head(&json, &out).status == SP5_OK);
    head[out.size] = 0;
    static const char policy[] =
        "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; "
        "object-src 'none'; base-uri 'none'; frame-ancestors 'none'\r\n";
    const char *required[] = {"HTTP/1.1 429 Too Many Requests\r\n",
                              "Content-Type: application/json\r\n",
                              "Content-Length: 17\r\n",
                              "Cache-Control: no-store\r\n",
                              "X-Content-Type-Options: nosniff\r\n",
                              policy,
                              "Referrer-Policy: no-referrer\r\n",
                              "Cross-Origin-Resource-Policy: same-origin\r\n",
                              "Connection: close\r\n\r\n"};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i)
        CHECK(strstr((char *)head, required[i]) != NULL);
    CHECK(strstr((char *)head, "Access-Control-") == NULL &&
          strstr((char *)head, "Transfer-Encoding") == NULL);
    sp5_web_response stream = {.status = 200,
                               .content_type = "application/x-ndjson",
                               .stream = sp5_metrics_web_export,
                               .download_filename = "metrics.ndjson"};
    CHECK(sp5_web_format_head(&stream, &out).status == SP5_OK);
    head[out.size] = 0;
    CHECK(strstr((char *)head, "HTTP/1.1 200 OK\r\n") != NULL &&
          strstr((char *)head, "Transfer-Encoding: chunked\r\n") != NULL &&
          strstr((char *)head,
                 "Content-Disposition: attachment; filename=\"metrics.ndjson\"\r\n") != NULL &&
          strstr((char *)head, "Content-Length") == NULL &&
          strstr((char *)head, "frame-ancestors 'none'") != NULL);
    stream.download_filename = "x\"\r\nSet-Cookie: a=b";
    CHECK(sp5_web_format_head(&stream, &out).status == SP5_ERROR);
    json.content_type = "text/html\r\nX-Injected: 1";
    CHECK(sp5_web_format_head(&json, &out).status == SP5_ERROR);
    close_fixture(f);
    return 0;
}

/* Extension routes: read-only refusals use the contract's 400 and report the
 * dispatch request identifier, not a clock value; nothing changes state. */
static int extension_errors(void)
{
    fixture *f = fresh_fixture();
    static sp5_fake_platform fake_storage;
    sp5_fake_platform *fake = &fake_storage;
    sp5_fake_init(fake);
    sp5_metrics *engine = sp5_metrics_create(7, (sp5_metrics_capacity){16, 16, 8}, &fake->clock);
    CHECK(engine != NULL);
    sp5_metrics_web metrics;
    sp5_diagnostics diagnostics;
    CHECK(sp5_metrics_web_init(&metrics, engine, false, 0).status == SP5_OK);
    CHECK(sp5_diagnostics_init(&diagnostics, engine, "unit", "synthetic").status == SP5_OK);
    f->extensions = (sp5_web_extensions){.context = &metrics, .route = sp5_metrics_web_route};
    CHECK(open_fixture(f, UINT64_C(77000000)) == 0);
    const char *paths[] = {"/api/v1/metrics", "/api/v1/metrics/export"};
    for (size_t i = 0; i < 2; ++i) {
        CHECK(api(f, "POST", paths[i], "{\"enabled\":true}") == 0);
        CHECK(f->result.status == 400 && has(f, "\"code\":\"read_only\""));
        char expected[64];
        int n = snprintf(expected, sizeof(expected), "\"request_id\":%" PRIu64 "}",
                         f->result.request_id);
        CHECK(n > 0 && (size_t)n < sizeof(expected) && has(f, expected));
        CHECK(f->result.request_id != 0 && f->result.request_id < 100 && !has(f, "77000000"));
        CHECK(!atomic_load(&metrics.requested) && !sp5_metrics_enabled(engine));
    }
    uint8_t body[256];
    sp5_web_response direct = {.body = {body, sizeof(body), 0}, .request_id = 41};
    sp5_request post = {.method = text("DELETE"), .target = text("/api/v1/diagnostics")};
    CHECK(sp5_diagnostics_route(&diagnostics, &post, 99000000, &direct));
    CHECK(direct.status == 400 && direct.body.size < sizeof(body));
    body[direct.body.size] = 0;
    CHECK(strstr((char *)body, "\"request_id\":41}") != NULL);
    close_fixture(f);
    sp5_metrics_destroy(engine);
    return 0;
}

int main(void)
{
    return journey() || long_names() || tokens() || expired_token() || assets() ||
           extension_errors();
}
