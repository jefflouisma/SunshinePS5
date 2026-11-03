#include "../fakes/platform.h"
#include "../fakes/web_service.h"
#include "protocol/discovery.h"
#include "session/revoke.h"
#include "web/server.h"
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs(__FILE__ ": " #c "\n", stderr) == EOF)                                       \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static sp5_bytes text(const char *s)
{
    return (sp5_bytes){(const uint8_t *)s, strlen(s)};
}
typedef struct {
    uint8_t bytes[SP5_STORE_FILE_COUNT][40000];
    size_t sizes[SP5_STORE_FILE_COUNT];
    bool fail, authorized_during_write;
    sp5_session *session;
    atomic_int handshake;
    bool concurrent_reader, reader_denied, other_lease_valid;
    sp5_device_lease other_lease;
} disk;
typedef struct {
    disk storage;
    sp5_store *store;
    sp5_host host;
    sp5_authority *authority;
    sp5_session *session, *other;
    sp5_web *web;
    sp5_pairing *pairing;
    char token[65], id[65];
    uint8_t response[SP5_WEB_RESPONSE_LIMIT];
    sp5_web_response result;
} fixture;
static sp5_result read_file(void *p, sp5_store_file f, sp5_buffer *out)
{
    disk *d = p;
    return d->sizes[f] ? sp5_buffer_append(out, (sp5_bytes){d->bytes[f], d->sizes[f]})
                       : sp5_failure(SP5_REASON_WOULD_BLOCK);
}
static sp5_result write_file(void *p, sp5_store_file f, sp5_bytes bytes)
{
    disk *d = p;
    if (d->session)
        d->authorized_during_write = sp5_session_authorized(d->session, text("A"));
    if (d->concurrent_reader) {
        atomic_store_explicit(&d->handshake, 1, memory_order_release);
        while (atomic_load_explicit(&d->handshake, memory_order_acquire) != 2)
            sched_yield();
    }
    if (d->fail || bytes.size > sizeof(d->bytes[f]))
        return sp5_failure(SP5_REASON_PLATFORM);
    memcpy(d->bytes[f], bytes.data, bytes.size);
    d->sizes[f] = bytes.size;
    return sp5_success();
}
static sp5_result identity(void *p, sp5_buffer *out)
{
    (void)p;
    return sp5_buffer_append(out, text("synthetic-unit-identity"));
}
typedef struct {
    const char *token, *peer, *host, *origin;
} request_context;
static int request(fixture *f, const char *method, const char *path, const char *body,
                   request_context context)
{
    char raw[4096];
    int n = snprintf(raw, sizeof(raw),
                     "%s %s HTTP/1.1\r\nHost: %s\r\n%s%s%sContent-Type: "
                     "application/json\r\nX-SP5-Token: %s\r\nContent-Length: %zu\r\n\r\n%s",
                     method, path, context.host, context.origin ? "Origin: " : "",
                     context.origin ? context.origin : "", context.origin ? "\r\n" : "",
                     context.token ? context.token : "", strlen(body), body);
    CHECK(n > 0 && (size_t)n < sizeof(raw));
    f->result = (sp5_web_response){.body = {f->response, sizeof(f->response) - 1, 0}};
    CHECK(sp5_web_dispatch(f->web, context.peer, false, text(raw), UINT64_C(5000000), &f->result)
              .status == SP5_OK);
    f->response[f->result.body.size] = 0;
    return 0;
}
static int call(fixture *f, const char *method, const char *path, const char *body)
{
    return request(f, method, path, body,
                   (request_context){f->token, "192.168.80.4", "console.local:8080",
                                     "http://console.local:8080"});
}
static int page_token(fixture *f)
{
    CHECK(call(f, "GET", "/api/v1/page", "") == 0 && f->result.status == 200);
    const char *start = strstr((char *)f->response, "\"token\":\"");
    CHECK(start);
    memcpy(f->token, start + 9, 64);
    f->token[64] = 0;
    return 0;
}
static int init(fixture *f)
{
    memset(f, 0, sizeof(*f));
    atomic_init(&f->storage.handshake, 0);
    sp5_storage_ops ops = {&f->storage, read_file, write_file, identity};
    f->store = sp5_store_create(&ops);
    CHECK(f->store);
    CHECK(sp5_store_load(f->store).status == SP5_OK);
    CHECK(sp5_host_init(&f->host, "portable-test", "13.60", "192.168.80.1").status == SP5_OK);
    CHECK(sp5_host_transition(&f->host, SP5_HOST_RUNNING).status == SP5_OK);
    f->host.supported_mode_count = 1;
    f->host.supported_modes[0] = (sp5_host_mode){1280, 720, 60, "fixture", SP5_SDR, 100, 1000};
    sp5_devices *d = sp5_store_devices(f->store);
    CHECK(sp5_devices_pair(d, text("A"), text("Tablet <img>"), 100).status == SP5_OK);
    CHECK(sp5_devices_pair(d, text("B"), text("Phone"), 101).status == SP5_OK);
    CHECK(sp5_devices_session_ended(d, text("B"), 701).status == SP5_OK);
    memcpy(f->id, sp5_devices_at(d, 0)->id, 65);
    f->authority = sp5_authority_create(d);
    CHECK(f->authority);
    f->session = sp5_session_create(f->authority);
    f->other = sp5_session_create(f->authority);
    CHECK(f->session && f->other);
    f->pairing = sp5_pairing_create();
    CHECK(f->pairing);
    sp5_web_config config = {.store = f->store,
                             .host = &f->host,
                             .session = f->session,
                             .pairing = f->pairing,
                             .authority = "console.local:8080"};
    f->web = sp5_web_create(&config);
    CHECK(f->web);
    return page_token(f);
}
/* Replaces the served web object (same fixture registry) and takes a fresh page token. */
static int serve(fixture *f, const sp5_web_config *config)
{
    sp5_web_destroy(f->web);
    f->web = sp5_web_create(config);
    CHECK(f->web);
    return page_token(f);
}
static void close_fixture(fixture *f)
{
    sp5_web_destroy(f->web);
    sp5_pairing_destroy(f->pairing);
    sp5_session_destroy(f->other);
    sp5_session_destroy(f->session);
    sp5_authority_destroy(f->authority);
    sp5_store_destroy(f->store);
}
static int start(fixture *f)
{
    CHECK(sp5_session_admit(f->session, text("A"), 1).status == SP5_OK);
    sp5_session_parameters p = {.requested_kbps = 5000, .started_us = 1000000};
    CHECK(sp5_session_configure(f->session, &f->host, &p).status == SP5_OK);
    CHECK(sp5_session_activate(f->session).status == SP5_OK);
    return 0;
}
static sp5_result capture_discovery(void *context, const sp5_discovery_record *record)
{
    sp5_discovery_record *output = context;
    *output = *record;
    return sp5_success();
}
static sp5_result withdraw_discovery(void *context)
{
    (void)context;
    return sp5_success();
}
static int management(void)
{
    fixture *f = calloc(1, sizeof(*f));
    CHECK(f && init(f) == 0);
    CHECK(call(f, "GET", "/api/v1/status", "") == 0 && f->result.status == 200);
    CHECK(strstr((char *)f->response, "\"session\":null"));
    CHECK(start(f) == 0);
    CHECK(call(f, "GET", "/api/v1/status", "") == 0 && f->result.status == 200);
    CHECK(strstr((char *)f->response, "\"requested_bitrate_kbps\":5000"));
    CHECK(strstr((char *)f->response, "\"chosen_bitrate_kbps\":1000"));
    CHECK(strstr((char *)f->response, "lowered_to_maximum"));
    CHECK(strstr((char *)f->response, "\"duration_us\":4000000"));
    CHECK(strstr((char *)f->response, "\"measured_bitrate_bps\":null"));
    char path[100];
    CHECK(snprintf(path, sizeof(path), "/api/v1/devices/%s", f->id) > 0);
    f->storage.fail = true;
    CHECK(call(f, "PATCH", path, "{\"name\":\"New name\"}") == 0 && f->result.status == 500);
    CHECK(strcmp(sp5_devices_at(sp5_store_devices(f->store), 0)->name, "Tablet <img>") == 0);
    f->storage.fail = false;
    CHECK(call(f, "PATCH", path, "{\"name\":\"New \\uD83C\\uDFAE\"}") == 0 &&
          f->result.status == 200);
    CHECK(strcmp(sp5_devices_at(sp5_store_devices(f->store), 0)->name, "New \xf0\x9f\x8e\xae") ==
          0);
    CHECK(call(f, "GET", "/api/v1/devices", "") == 0 && f->result.status == 200);
    CHECK(strstr((char *)f->response, "\"paired_at\":100"));
    CHECK(strstr((char *)f->response, "\"last_session_at\":null"));
    CHECK(strstr((char *)f->response, "\"last_session_at\":701"));
    CHECK(!strstr((char *)f->response, "certificate"));
    CHECK(call(f, "PATCH", "/api/v1/settings", "{\"metrics_enabled\":true}") == 0 &&
          f->result.status == 200);
    CHECK(call(f, "PATCH", "/api/v1/settings", "{\"host_name\":\"Den\"}") == 0 &&
          f->result.status == 200);
    CHECK(sp5_store_settings(f->store).metrics_enabled &&
          strcmp(f->host.settings.host_name, "Den") == 0);
    sp5_discovery_record advertised = {0};
    sp5_discovery discovery;
    CHECK(sp5_discovery_init(
              &discovery, (sp5_discovery_ops){&advertised, capture_discovery, withdraw_discovery})
              .status == SP5_OK);
    CHECK(sp5_discovery_refresh(&discovery, &f->host, 1, 47989).status == SP5_OK);
    CHECK(strcmp(advertised.name, "Den") == 0);
    CHECK(sp5_discovery_stop(&discovery).status == SP5_OK);
    f->storage.fail = true;
    CHECK(call(f, "PATCH", "/api/v1/settings",
               "{\"host_name\":\"Failed\",\"metrics_enabled\":false}") == 0 &&
          f->result.status == 500);
    CHECK(sp5_store_settings(f->store).metrics_enabled &&
          strcmp(f->host.settings.host_name, "Den") == 0);
    CHECK(call(f, "DELETE", path, "{\"confirm\":false}") == 0 && f->result.status == 400);
    CHECK(sp5_session_authorized(f->session, text("A")));
    f->storage.session = f->session;
    CHECK(call(f, "DELETE", path, "{\"confirm\":true}") == 0 && f->result.status == 500);
    CHECK(strstr((char *)f->response, "restart"));
    CHECK(!f->storage.authorized_during_write);
    CHECK(!sp5_store_authorized(f->store, text("A")) && sp5_store_authorized(f->store, text("B")));
    CHECK(!sp5_session_authorized(f->session, text("A")) &&
          sp5_session_read(f->session).end_reason == SP5_END_REVOKED);
    CHECK(sp5_session_admit(f->session, text("A"), 2).status == SP5_ERROR);
    CHECK(sp5_session_admit(f->other, text("B"), 2).status == SP5_OK);
    CHECK(sp5_session_admit(f->session, text("A"), 3).status == SP5_ERROR);
    CHECK(sp5_session_admit(f->session, text("B"), 3).status == SP5_ERROR);
    close_fixture(f);
    free(f);
    return 0;
}
static int hostile(void)
{
    fixture *f = calloc(1, sizeof(*f));
    CHECK(f && init(f) == 0);
    uint64_t pairing_id = 0;
    CHECK(sp5_pairing_begin(f->pairing, 1, &pairing_id).status == SP5_OK);
    CHECK(call(f, "GET", "/api/v1/pairing", "") == 0 && f->result.status == 200);
    CHECK(strstr((char *)f->response, "\"id\":\""));
    char pin_body[128];
    CHECK(snprintf(pin_body, sizeof(pin_body), "{\"request_id\":\"%" PRIu64 "\",\"pin\":\"1234\"}",
                   pairing_id) > 0);
    CHECK(call(f, "POST", "/api/v1/pairing/pin", pin_body) == 0 && f->result.status == 503);
    CHECK(request(f, "GET", "/api/v1/status", "",
                  (request_context){NULL, "8.8.8.8", "console.local:8080", NULL}) == 0 &&
          f->result.status == 403);
    CHECK(request(f, "GET", "/api/v1/status", "",
                  (request_context){NULL, "192.168.2.2", "evil.test", NULL}) == 0 &&
          f->result.status == 403);
    CHECK(request(f, "GET", "/api/v1/status", "",
                  (request_context){NULL, "192.168.2.2", "console.local:8080",
                                    "https://evil.test"}) == 0 &&
          f->result.status == 403);
    CHECK(request(f, "PATCH", "/api/v1/settings", "{\"host_name\":\"Bad\"}",
                  (request_context){"stale", "192.168.2.2", "console.local:8080", NULL}) == 0 &&
          f->result.status == 403);
    CHECK(request(f, "PATCH", "/api/v1/settings", "{\"host_name\":\"Good\"}",
                  (request_context){f->token, "fd12::10", "console.local:8080", NULL}) == 0 &&
          f->result.status == 200);
    const char *invalid[] = {
        "{}",
        "{\"host_name\":\"\"}",
        "{\"host_name\":\"x\\u0000y\"}",
        "{\"host_name\":\"x\\u0085y\"}",
        "{\"host_name\":\"A\",\"host_name\":\"B\"}",
        "{\"host_name\":\"A\",\"extra\":true}",
        "{\"metrics_enabled\":1}",
        "{\"host_name\":\"\\ud800\"}",
        "{\"host_name\":\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijkl\"}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        CHECK(call(f, "PATCH", "/api/v1/settings", invalid[i]) == 0 && f->result.status == 400);
    CHECK(call(f, "POST", "/api/v1/host/stop", "{\"confirm\":false}") == 0 &&
          f->result.status == 400);
    CHECK(call(f, "POST", "/api/v1/host/stop", "{\"confirm\":true}") == 0 &&
          f->result.status == 200 && f->result.stopping);
    CHECK(strstr((char *)f->response, "stopping") && !strstr((char *)f->response, "stopped"));
    CHECK(call(f, "PATCH", "/api/v1/settings", "{\"host_name\":\"Late\"}") == 0 &&
          f->result.status == 409);
    CHECK(sp5_web_finish_stop(f->web).status == SP5_OK);
    close_fixture(f);
    free(f);
    return 0;
}
static void *authorization_reader(void *context)
{
    disk *d = context;
    while (atomic_load_explicit(&d->handshake, memory_order_acquire) != 1)
        sched_yield();
    d->reader_denied = !sp5_session_authorized(d->session, text("A"));
    d->other_lease_valid = sp5_device_lease_valid(d->other_lease);
    atomic_store_explicit(&d->handshake, 2, memory_order_release);
    return NULL;
}
static int concurrent_and_restart(void)
{
    fixture *f = calloc(1, sizeof(*f));
    CHECK(f && init(f) == 0 && start(f) == 0);
    disk *d = &f->storage;
    d->session = f->session;
    d->concurrent_reader = true;
    d->other_lease = sp5_devices_lease(sp5_store_devices(f->store), text("B"));
    pthread_t reader;
    CHECK(pthread_create(&reader, NULL, authorization_reader, d) == 0);
    char path[100];
    CHECK(snprintf(path, sizeof(path), "/api/v1/devices/%s", f->id) > 0);
    CHECK(call(f, "DELETE", path, "{\"confirm\":true}") == 0 && f->result.status == 200);
    CHECK(pthread_join(reader, NULL) == 0);
    CHECK(d->reader_denied && d->other_lease_valid && !d->authorized_during_write);
    d->concurrent_reader = false;
    sp5_storage_ops ops = {d, read_file, write_file, identity};
    sp5_store *restarted = sp5_store_create(&ops);
    CHECK(restarted && sp5_store_load(restarted).status == SP5_OK);
    CHECK(!sp5_store_authorized(restarted, text("A")) &&
          sp5_store_authorized(restarted, text("B")));
    sp5_store_destroy(restarted);
    CHECK(call(f, "DELETE", path, "{\"confirm\":true}") == 0 && f->result.status == 404);
    char stale[65];
    memcpy(stale, f->token, sizeof(stale));
    sp5_web_destroy(f->web);
    sp5_web_config config = {.store = f->store,
                             .host = &f->host,
                             .session = f->session,
                             .authority = "console.local:8080"};
    f->web = sp5_web_create(&config);
    CHECK(f->web);
    CHECK(call(f, "PATCH", "/api/v1/settings", "{\"host_name\":\"Stale page\"}") == 0 &&
          f->result.status == 403);
    CHECK(strcmp(stale, f->token) == 0);
    close_fixture(f);
    free(f);
    return 0;
}
#define BODY(f) ((const char *)(f)->response)
/* T050/FR-039: store recovery diagnostics reach the owner with a next step. */
static int storage_notice(void)
{
    fixture *f = calloc(1, sizeof(*f));
    CHECK(f && init(f) == 0);
    CHECK(call(f, "GET", "/api/v1/status", "") == 0 && f->result.status == 200);
    CHECK(strstr(BODY(f), "\"storage\":{\"settings_recovered\":false,\"repairing_required\":false,"
                          "\"revocation_durability_pending\":false,\"diagnostic\":\"none\","
                          "\"explanation\":\"\"}"));
    CHECK(call(f, "PATCH", "/api/v1/settings",
               "{\"host_name\":\"Den\",\"metrics_enabled\":true}") == 0 &&
          f->result.status == 200);
    disk *d = &f->storage;
    static uint8_t original[40000];
    for (sp5_store_file file = SP5_STORE_SETTINGS; file <= SP5_STORE_DEVICES; ++file) {
        size_t original_size = d->sizes[file];
        CHECK(original_size > 7);
        memcpy(original, d->bytes[file], original_size);
        /* A truncated record through the fake storage, as after a torn write. */
        d->sizes[file] = original_size / 2;
        sp5_storage_ops ops = {d, read_file, write_file, identity};
        sp5_store *loaded = sp5_store_create(&ops);
        CHECK(loaded && sp5_store_load(loaded).status == SP5_OK);
        sp5_web_config config = {
            .store = loaded, .host = &f->host, .authority = "console.local:8080"};
        CHECK(serve(f, &config) == 0);
        CHECK(call(f, "GET", "/api/v1/status", "") == 0 && f->result.status == 200);
        CHECK(strstr(BODY(f), "\"diagnostic\":\"invalid_argument\""));
        if (file == SP5_STORE_SETTINGS) {
            CHECK(strstr(BODY(f), "\"settings_recovered\":true,\"repairing_required\":false"));
            CHECK(strstr(BODY(f), "Saved settings were unreadable; defaults are in use. Re-enter "
                                  "the host name and metrics setting."));
            CHECK(strstr(BODY(f), "\"settings\":{\"host_name\":\"SunshinePS5\","
                                  "\"metrics_enabled\":false"));
            CHECK(sp5_store_authorized(loaded, text("A")));
            /* Saving settings again replaces the defaults, so the notice clears. */
            CHECK(call(f, "PATCH", "/api/v1/settings", "{\"host_name\":\"Den\"}") == 0 &&
                  f->result.status == 200);
            CHECK(call(f, "GET", "/api/v1/status", "") == 0 && f->result.status == 200);
            CHECK(strstr(BODY(f), "\"settings_recovered\":false,\"repairing_required\":false"));
        } else {
            CHECK(strstr(BODY(f), "\"settings_recovered\":false,\"repairing_required\":true"));
            CHECK(strstr(BODY(f), "no devices are authorized. Pair again."));
            CHECK(strstr(BODY(f), "\"host_name\":\"Den\""));
            CHECK(!sp5_store_authorized(loaded, text("A")) &&
                  !sp5_store_authorized(loaded, text("B")));
        }
        sp5_web_config restored = {
            .store = f->store, .host = &f->host, .authority = "console.local:8080"};
        CHECK(serve(f, &restored) == 0);
        sp5_store_destroy(loaded);
        memcpy(d->bytes[file], original, original_size);
        d->sizes[file] = original_size;
    }
    close_fixture(f);
    free(f);
    return 0;
}
/* T094: a revocation that could not persist stays visible and retryable. */
static int revocation_durability(void)
{
    fixture *f = calloc(1, sizeof(*f));
    CHECK(f && init(f) == 0);
    char path[100];
    CHECK(snprintf(path, sizeof(path), "/api/v1/devices/%s", f->id) > 0);
    CHECK(call(f, "GET", "/api/v1/devices", "") == 0 && f->result.status == 200);
    CHECK(strstr(BODY(f), "\"revocation_durability_pending\":false,\"pending_revocations\":[]"));
    f->storage.fail = true;
    CHECK(call(f, "DELETE", path, "{\"confirm\":true}") == 0 && f->result.status == 500);
    CHECK(strstr(BODY(f), "Restart protection is not confirmed"));
    CHECK(!sp5_store_authorized(f->store, text("A")) && sp5_store_authorized(f->store, text("B")));
    CHECK(call(f, "GET", "/api/v1/devices", "") == 0 && f->result.status == 200);
    char pending[256];
    CHECK(snprintf(pending, sizeof(pending),
                   "\"revocation_durability_pending\":true,\"pending_revocations\":[{\"id\":\"%s\","
                   "\"name\":\"Tablet <img>\"}]",
                   f->id) > 0);
    CHECK(strstr(BODY(f), pending));
    CHECK(!strstr(BODY(f), "certificate"));
    CHECK(call(f, "GET", "/api/v1/status", "") == 0 &&
          strstr(BODY(f), "\"revocation_durability_pending\":true"));
    /* Retrying while storage still fails keeps the explicit failure, not a 404. */
    CHECK(call(f, "DELETE", path, "{\"confirm\":true}") == 0 && f->result.status == 500);
    CHECK(strstr(BODY(f), "Restart protection is not confirmed"));
    CHECK(call(f, "DELETE", path, "{\"confirm\":false}") == 0 && f->result.status == 400);
    CHECK(call(f, "PATCH", path, "{\"name\":\"Back\"}") == 0 && f->result.status == 404);
    f->storage.fail = false;
    CHECK(call(f, "DELETE", path, "{\"confirm\":true}") == 0 && f->result.status == 200);
    CHECK(strstr(BODY(f), "\"state\":\"revoked\",\"durable\":true"));
    CHECK(call(f, "GET", "/api/v1/devices", "") == 0 && f->result.status == 200);
    CHECK(strstr(BODY(f), "\"revocation_durability_pending\":false,\"pending_revocations\":[]"));
    CHECK(call(f, "DELETE", path, "{\"confirm\":true}") == 0 && f->result.status == 404);
    sp5_storage_ops ops = {&f->storage, read_file, write_file, identity};
    sp5_store *restarted = sp5_store_create(&ops);
    CHECK(restarted && sp5_store_load(restarted).status == SP5_OK);
    CHECK(!sp5_store_authorized(restarted, text("A")) &&
          sp5_store_authorized(restarted, text("B")));
    sp5_store_destroy(restarted);
    /* Any later successful registry write also makes a pending removal durable. */
    sp5_devices *devices = sp5_store_devices(f->store);
    char other[65];
    memcpy(other, sp5_devices_at(devices, 0)->id, sizeof(other));
    CHECK(snprintf(path, sizeof(path), "/api/v1/devices/%s", other) > 0);
    f->storage.fail = true;
    CHECK(call(f, "DELETE", path, "{\"confirm\":true}") == 0 && f->result.status == 500);
    CHECK(sp5_devices_durability_pending(devices) && sp5_devices_pending_count(devices) == 1);
    f->storage.fail = false;
    CHECK(sp5_devices_pair(devices, text("C"), text("Laptop"), 900).status == SP5_OK);
    CHECK(!sp5_devices_durability_pending(devices) && sp5_devices_pending_count(devices) == 0);
    CHECK(sp5_devices_persist_pending(devices).status == SP5_OK);
    restarted = sp5_store_create(&ops);
    CHECK(restarted && sp5_store_load(restarted).status == SP5_OK);
    CHECK(!sp5_store_authorized(restarted, text("B")) &&
          sp5_store_authorized(restarted, text("C")));
    sp5_store_destroy(restarted);
    close_fixture(f);
    free(f);
    return 0;
}
static uint64_t monotonic_us(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return 0;
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}
/* Session for A bound to a lifecycle on the fake platform, two controllers held. */
static sp5_lifecycle *live_with_audio_close(fixture *f, sp5_fake_platform *fake,
                                            sp5_result (*audio_close)(void *))
{
    sp5_fake_init(fake);
    sp5_lifecycle_ops ops = {fake->capture, fake->encoder, fake->audio, fake->input, fake->system};
    if (audio_close != NULL)
        ops.audio.close = audio_close;
    sp5_lifecycle *l = sp5_lifecycle_create(1, &ops);
    sp5_lifecycle_policy policy = {.mode_count = 1,
                                   .media_bytes = 4096,
                                   .core_bytes = 4096,
                                   .streaming_cpu_us_per_10s = 100,
                                   .idle_cpu_us_per_10s = 1,
                                   .validity = SP5_VALIDITY_VALID,
                                   .synthetic = true,
                                   .evidence_ref = "unit:synthetic-only",
                                   .clock = fake->clock,
                                   .metrics_capacity = {64, 64, 8}};
    policy.modes[0] = f->host.supported_modes[0];
    policy.metrics_context.synthetic = true;
    sp5_session_parameters parameters = {.requested_kbps = 5000, .started_us = 1000000};
    sp5_media_config media = {1280, 720, 60, 1, "fixture", SP5_SDR, 1000};
    if (!l || sp5_session_admit(f->session, text("A"), 1).status != SP5_OK ||
        sp5_session_configure(f->session, &f->host, &parameters).status != SP5_OK ||
        sp5_lifecycle_configure(l, &policy).status != SP5_OK ||
        sp5_lifecycle_bind(l, f->session, text("A")).status != SP5_OK ||
        sp5_lifecycle_start(l, &media, 1000000).status != SP5_OK ||
        sp5_session_activate(f->session).status != SP5_OK) {
        sp5_lifecycle_destroy(l);
        return NULL;
    }
    for (uint32_t id = 0; id < 2; ++id) {
        sp5_controller_state state = {.buttons = 0xffff, .triggers = {65535, 1}};
        sp5_input_evidence evidence = {.session_id = 1, .controller_id = id};
        if (fake->input.add(fake, 1, id).status != SP5_OK ||
            fake->input.deliver(fake, &state, false, &evidence).status != SP5_OK) {
            sp5_lifecycle_destroy(l);
            return NULL;
        }
    }
    return l;
}
static sp5_lifecycle *live(fixture *f, sp5_fake_platform *fake)
{
    return live_with_audio_close(f, fake, NULL);
}
static bool neutral(const sp5_fake_platform *fake)
{
    for (size_t i = 0; i < SP5_MAX_CONTROLLERS; ++i)
        if (fake->controllers[i].live || fake->controllers[i].state.buttons != 0 ||
            fake->controllers[i].state.triggers[0] != 0 || fake->controllers[i].state.triggers[1])
            return false;
    return true;
}
static bool media_closed(const sp5_fake_platform *fake)
{
    return !fake->capture_open && !fake->encoder_open && !fake->audio_open && !fake->prepared &&
           fake->rollbacks == 1 && fake->last_input_release_sequence > 0 &&
           fake->last_input_release_sequence < fake->last_media_close_sequence;
}
/* Re-initialized per case; never copied while a lifecycle borrows its operations. */
static sp5_fake_platform fake_platform;
/* T094/T096: core/session/revoke.c and web stop/finish with a bound lifecycle. */
static int bound_lifecycle(void)
{
    for (int failing = 0; failing < 2; ++failing) {
        fixture *f = calloc(1, sizeof(*f));
        sp5_fake_platform *fake = &fake_platform;
        CHECK(f && init(f) == 0);
        sp5_lifecycle *l = live(f, fake);
        CHECK(l && !neutral(fake) && fake->capture_open && fake->audio_open);
        sp5_web_config config = {.store = f->store,
                                 .host = &f->host,
                                 .session = f->session,
                                 .lifecycle = l,
                                 .pairing = f->pairing,
                                 .authority = "console.local:8080"};
        CHECK(serve(f, &config) == 0);
        char path[100];
        CHECK(snprintf(path, sizeof(path), "/api/v1/devices/%s", f->id) > 0);
        f->storage.fail = failing != 0;
        uint64_t began = monotonic_us();
        CHECK(call(f, "DELETE", path, "{\"confirm\":true}") == 0);
        uint64_t elapsed = monotonic_us() - began;
        CHECK(f->result.status == (failing ? 500U : 200U));
        /* Cleanup is complete before the response: synchronous, portable, < 2 s. */
        CHECK(elapsed < UINT64_C(2000000));
        CHECK(neutral(fake) && media_closed(fake));
        sp5_session_view view = sp5_lifecycle_read(l);
        CHECK(view.state == SP5_SESSION_ENDED && view.end_reason == SP5_END_REVOKED);
        CHECK(sp5_session_read(f->session).end_reason == SP5_END_REVOKED);
        CHECK(!sp5_store_authorized(f->store, text("A")) &&
              sp5_store_authorized(f->store, text("B")));
        CHECK(call(f, "GET", "/api/v1/status", "") == 0 && strstr(BODY(f), "\"session\":null"));
        sp5_web_destroy(f->web);
        f->web = NULL;
        sp5_lifecycle_destroy(l);
        close_fixture(f);
        free(f);
    }
    for (int failing = 0; failing < 2; ++failing) {
        fixture *f = calloc(1, sizeof(*f));
        sp5_fake_platform *fake = &fake_platform;
        CHECK(f && init(f) == 0);
        sp5_lifecycle *l = live(f, fake);
        CHECK(l);
        sp5_web_config config = {.store = f->store,
                                 .host = &f->host,
                                 .session = f->session,
                                 .lifecycle = l,
                                 .pairing = f->pairing,
                                 .authority = "console.local:8080"};
        CHECK(serve(f, &config) == 0);
        CHECK(call(f, "POST", "/api/v1/host/stop", "{\"confirm\":true}") == 0);
        CHECK(f->result.status == 200 && f->result.stopping);
        /* Acknowledged: input already released while media are still open. */
        CHECK(neutral(fake) && fake->capture_open && fake->encoder_open && fake->audio_open);
        CHECK(fake->last_input_release_sequence > 0 && fake->last_media_close_sequence == 0);
        CHECK(!sp5_lifecycle_input_open(l));
        if (failing) {
            fake->fail_next[SP5_FAKE_AUDIO_CLOSE] = SP5_REASON_PLATFORM;
            sp5_result first = sp5_web_finish_stop(f->web);
            CHECK(first.status == SP5_ERROR && first.reason == SP5_REASON_PLATFORM);
            CHECK(fake->audio_open && sp5_lifecycle_read(l).state != SP5_SESSION_ENDED);
            CHECK(sp5_lifecycle_cleanup_diagnostic(l) == SP5_REASON_PLATFORM);
        }
        CHECK(sp5_web_finish_stop(f->web).status == SP5_OK);
        CHECK(neutral(fake) && media_closed(fake));
        sp5_session_view view = sp5_lifecycle_read(l);
        CHECK(view.state == SP5_SESSION_ENDED && view.end_reason == SP5_END_HOST_STOPPING);
        CHECK(sp5_store_authorized(f->store, text("A")) &&
              sp5_store_authorized(f->store, text("B")));
        sp5_web_destroy(f->web);
        f->web = NULL;
        sp5_lifecycle_destroy(l);
        close_fixture(f);
        free(f);
    }
    return 0;
}
/* A removed registry entry must not strand failed lifecycle cleanup. */
static int revocation_cleanup_retry(void)
{
    for (int scenario = 0; scenario < 4; ++scenario) {
        int storage_failure = scenario % 2;
        bool prior_end = scenario >= 2;
        fixture *f = calloc(1, sizeof(*f));
        sp5_fake_platform *fake = &fake_platform;
        CHECK(f && init(f) == 0);
        sp5_lifecycle *l = live(f, fake);
        CHECK(l);
        static sp5_fake_platform unrelated;
        sp5_fake_init(&unrelated);
        sp5_lifecycle_ops unrelated_ops = {unrelated.capture, unrelated.encoder, unrelated.audio,
                                           unrelated.input, unrelated.system};
        sp5_lifecycle *other_lifecycle = sp5_lifecycle_create(2, &unrelated_ops);
        CHECK(other_lifecycle);
        bool persisted = true;
        sp5_result wrong_owner = sp5_revoke_device(sp5_store_devices(f->store), f->session,
                                                   other_lifecycle, f->id, &persisted);
        CHECK(wrong_owner.status == SP5_ERROR && wrong_owner.reason == SP5_REASON_OWNERSHIP);
        CHECK(!persisted && sp5_store_authorized(f->store, text("A")));
        CHECK(sp5_lifecycle_input_open(l) && fake->calls[SP5_FAKE_AUDIO_CLOSE] == 0);
        sp5_lifecycle_destroy(other_lifecycle);
        sp5_web_config config = {.store = f->store,
                                 .host = &f->host,
                                 .session = f->session,
                                 .lifecycle = l,
                                 .pairing = f->pairing,
                                 .authority = "console.local:8080"};
        CHECK(serve(f, &config) == 0);
        char path[100];
        CHECK(snprintf(path, sizeof(path), "/api/v1/devices/%s", f->id) > 0);
        f->storage.session = f->session;
        f->storage.fail = storage_failure != 0;
        CHECK(call(f, "GET", "/api/v1/devices", "") == 0 && f->result.status == 200);
        CHECK(strstr(BODY(f), "\"cleanup_pending_revocations\":[]"));
        if (prior_end)
            CHECK(sp5_lifecycle_stop_input(l, SP5_END_CLIENT_LOST).status == SP5_OK);
        CHECK(call(f, "GET", "/api/v1/devices", "") == 0 && f->result.status == 200);
        CHECK(strstr(BODY(f), "\"cleanup_pending_revocations\":[]"));
        fake->fail_next[SP5_FAKE_AUDIO_CLOSE] = SP5_REASON_PLATFORM;
        CHECK(call(f, "DELETE", path, "{\"confirm\":true}") == 0 && f->result.status == 500);
        CHECK(strstr(BODY(f), storage_failure ? "Restart protection is not confirmed"
                                              : "Removal persisted"));
        CHECK(!f->storage.authorized_during_write && !sp5_lifecycle_input_open(l));
        CHECK(neutral(fake) && fake->audio_open);
        CHECK(sp5_lifecycle_read(l).state == SP5_SESSION_ENDING);
        CHECK(sp5_session_read(f->session).state == SP5_SESSION_ENDING);
        CHECK(!sp5_store_authorized(f->store, text("A")) &&
              sp5_store_authorized(f->store, text("B")));
        CHECK(fake->calls[SP5_FAKE_AUDIO_CLOSE] == 1);
        CHECK(call(f, "GET", "/api/v1/devices", "") == 0 && f->result.status == 200);
        char pending_cleanup[256];
        CHECK(
            snprintf(pending_cleanup, sizeof(pending_cleanup),
                     "\"cleanup_pending_revocations\":[{\"id\":\"%s\",\"name\":\"Tablet <img>\"}]",
                     f->id) > 0);
        CHECK(strstr(BODY(f), pending_cleanup));
        CHECK(call(f, "DELETE", path, "{\"confirm\":false}") == 0 && f->result.status == 400);
        CHECK(call(f, "PATCH", path, "{\"name\":\"Back\"}") == 0 && f->result.status == 404);
        char unknown[100];
        memcpy(unknown, path, strlen(path) + 1);
        unknown[16] = unknown[16] == '0' ? '1' : '0';
        CHECK(call(f, "DELETE", unknown, "{\"confirm\":true}") == 0 && f->result.status == 404);
        CHECK(fake->calls[SP5_FAKE_AUDIO_CLOSE] == 1);
        f->storage.fail = false;
        /* A successful persistence retry must still expose another cleanup failure. */
        fake->fail_next[SP5_FAKE_AUDIO_CLOSE] = SP5_REASON_CAPACITY;
        CHECK(call(f, "DELETE", path, "{\"confirm\":true}") == 0 && f->result.status == 500);
        CHECK(strstr(BODY(f), "Removal persisted and access is blocked; session cleanup failed"));
        CHECK(!sp5_devices_durability_pending(sp5_store_devices(f->store)));
        CHECK(fake->audio_open && fake->calls[SP5_FAKE_AUDIO_CLOSE] == 2);
        CHECK(sp5_lifecycle_cleanup_diagnostic(l) == SP5_REASON_PLATFORM);
        /* Refresh retains a retry even after durable removal cleared its row. */
        CHECK(call(f, "GET", "/api/v1/devices", "") == 0 && f->result.status == 200);
        CHECK(
            strstr(BODY(f), "\"revocation_durability_pending\":false,\"pending_revocations\":[]"));
        CHECK(strstr(BODY(f), pending_cleanup));
        CHECK(call(f, "DELETE", path, "{\"confirm\":true}") == 0 && f->result.status == 200);
        CHECK(strstr(BODY(f), "\"state\":\"revoked\",\"durable\":true"));
        CHECK(neutral(fake) && media_closed(fake));
        CHECK(sp5_lifecycle_read(l).state == SP5_SESSION_ENDED);
        CHECK(sp5_session_read(f->session).state == SP5_SESSION_ENDED);
        sp5_end_reason expected_end = prior_end ? SP5_END_CLIENT_LOST : SP5_END_REVOKED;
        CHECK(sp5_lifecycle_read(l).end_reason == expected_end);
        CHECK(sp5_session_read(f->session).end_reason == expected_end);
        CHECK(sp5_lifecycle_cleanup_diagnostic(l) == SP5_REASON_PLATFORM);
        CHECK(fake->calls[SP5_FAKE_AUDIO_CLOSE] == 3 && fake->capture_closes == 1 &&
              fake->encoder_closes == 1 && fake->rollbacks == 1);
        CHECK(call(f, "GET", "/api/v1/devices", "") == 0 && f->result.status == 200);
        CHECK(strstr(BODY(f), "\"cleanup_pending_revocations\":[]"));
        CHECK(call(f, "DELETE", path, "{\"confirm\":true}") == 0 && f->result.status == 404);
        CHECK(fake->calls[SP5_FAKE_AUDIO_CLOSE] == 3);
        sp5_storage_ops ops = {&f->storage, read_file, write_file, identity};
        sp5_store *restarted = sp5_store_create(&ops);
        CHECK(restarted && sp5_store_load(restarted).status == SP5_OK);
        CHECK(!sp5_store_authorized(restarted, text("A")) &&
              sp5_store_authorized(restarted, text("B")));
        sp5_store_destroy(restarted);
        CHECK(
            printf(
                "revoke HTTP retry: initial_storage_failure=%d prior_end=%d; two cleanup failures "
                "retained; cleanup row survives refresh then clears; third attempt ended; "
                "restart denies A and preserves B\n",
                storage_failure, prior_end) > 0);
        sp5_web_destroy(f->web);
        f->web = NULL;
        sp5_lifecycle_destroy(l);
        close_fixture(f);
        free(f);
    }
    return 0;
}
static struct {
    int peer;
    const uint8_t *expected;
    size_t size;
    unsigned calls;
    bool acknowledged_before_close;
} stop_observer;
static sp5_result observe_audio_close(void *context)
{
    sp5_fake_platform *fake = context;
    uint8_t received[SP5_FAKE_WEB_WIRE_BYTES];
    ssize_t size = recv(stop_observer.peer, received, sizeof(received), MSG_PEEK | MSG_DONTWAIT);
    stop_observer.calls++;
    stop_observer.acknowledged_before_close =
        stop_observer.acknowledged_before_close && size == (ssize_t)stop_observer.size &&
        memcmp(received, stop_observer.expected, stop_observer.size) == 0;
    return fake->audio.close(context);
}
static int transport_stop_retry(void)
{
    static sp5_fake_web transport;
    fixture *f = calloc(1, sizeof(*f));
    sp5_fake_web *service = &transport;
    sp5_fake_platform *fake = &fake_platform;
    CHECK(f && init(f) == 0);
    sp5_lifecycle *l = live_with_audio_close(f, fake, observe_audio_close);
    CHECK(l);
    sp5_web_config config = {.store = f->store,
                             .host = &f->host,
                             .session = f->session,
                             .lifecycle = l,
                             .pairing = f->pairing,
                             .authority = "console.local:8080"};
    CHECK(serve(f, &config) == 0);
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    CHECK(fcntl(sockets[0], F_SETFL, O_NONBLOCK) == 0);
    sp5_fake_web_init(service, f->web, -1, "192.168.80.4");
    service->connections[0].fd = sockets[0];
    service->connections[0].deadline_us = UINT64_C(8000000);
    char raw[512];
    const char *body = "{\"confirm\":true}";
    int length = snprintf(raw, sizeof(raw),
                          "POST /api/v1/host/stop HTTP/1.1\r\nHost: console.local:8080\r\n"
                          "Origin: http://console.local:8080\r\nContent-Type: application/json\r\n"
                          "X-SP5-Token: %s\r\nContent-Length: %zu\r\n\r\n%s",
                          f->token, strlen(body), body);
    CHECK(length > 0 && (size_t)length < sizeof(raw));
    CHECK(send(sockets[1], raw, (size_t)length, 0) == length);
    struct pollfd polls[SP5_FAKE_WEB_POLLS];
    sp5_fake_web_poll(service, polls);
    CHECK(poll(polls, SP5_FAKE_WEB_POLLS, 1000) == 1);
    sp5_fake_web_step(service, polls, UINT64_C(5000000), false);
    CHECK(service->stopping && !service->stopped && !service->failed);
    CHECK(service->connections[0].size > 0 && service->connections[0].sent == 0);
    CHECK(neutral(fake) && fake->audio_open && fake->capture_open && fake->encoder_open);
    CHECK(fake->last_media_close_sequence == 0);
    CHECK(strstr((char *)service->connections[0].wire, "\"state\":\"stopping\""));
    stop_observer.peer = sockets[1];
    stop_observer.expected = service->connections[0].wire;
    stop_observer.size = service->connections[0].size;
    stop_observer.calls = 0;
    stop_observer.acknowledged_before_close = true;
    CHECK(puts("stop transport: acknowledgement queued; input neutral; media cleanup deferred") >=
          0);
    fake->fail_next[SP5_FAKE_AUDIO_CLOSE] = SP5_REASON_PLATFORM;
    sp5_fake_web_poll(service, polls);
    CHECK(poll(polls, SP5_FAKE_WEB_POLLS, 1000) == 1);
    sp5_fake_web_step(service, polls, UINT64_C(5000001), false);
    CHECK(stop_observer.calls == 1 && stop_observer.acknowledged_before_close);
    CHECK(puts("stop transport: complete acknowledgement observed by cleanup callback") >= 0);
    CHECK(service->stopping && !service->stopped && !service->failed);
    CHECK(service->cleanup_error == SP5_REASON_PLATFORM);
    CHECK(fake->audio_open && sp5_lifecycle_read(l).state == SP5_SESSION_ENDING);
    CHECK(!sp5_lifecycle_input_open(l));
    CHECK(sp5_lifecycle_cleanup_diagnostic(l) == SP5_REASON_PLATFORM);
    CHECK(puts("stop transport: full acknowledgement visible before failed audio cleanup; "
               "stopping and ownership retained") >= 0);
    sp5_fake_web_poll(service, polls);
    CHECK(poll(polls, SP5_FAKE_WEB_POLLS, 0) == 0);
    sp5_fake_web_step(service, polls, UINT64_C(5000002), false);
    CHECK(stop_observer.calls == 2 && stop_observer.acknowledged_before_close);
    CHECK(service->stopping && service->stopped && !service->failed);
    CHECK(service->cleanup_error == SP5_REASON_PLATFORM);
    CHECK(neutral(fake) && media_closed(fake));
    CHECK(sp5_lifecycle_read(l).state == SP5_SESSION_ENDED);
    CHECK(sp5_lifecycle_read(l).end_reason == SP5_END_HOST_STOPPING);
    sp5_fake_web_step(service, polls, UINT64_C(5000003), false);
    CHECK(stop_observer.calls == 2);
    CHECK(puts("stop transport: retry completed cleanup; stopped set only after success; "
               "completed cleanup not repeated") >= 0);
    sp5_fake_web_close(service);
    CHECK(!service->failed && close(sockets[1]) == 0);
    sp5_web_destroy(f->web);
    f->web = NULL;
    sp5_lifecycle_destroy(l);
    close_fixture(f);
    free(f);
    return 0;
}
int main(void)
{
    return management() || hostile() || concurrent_and_restart() || storage_notice() ||
           revocation_durability() || bound_lifecycle() || revocation_cleanup_retry() ||
           transport_stop_retry();
}
