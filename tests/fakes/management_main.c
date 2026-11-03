/* POSIX/browser fixture only. Loopback sockets map to a synthetic private peer
 * at the adapter boundary; core address policy is unchanged and separately
 * tested. Real core registry/storage/lifecycle; no PS5 or reference-host claim.
 * Usage: sp5_management_host STATE_DIR WEBAPP_DIR MODE. WEBAPP_DIR only guards
 * against a stale build: the core serves its embedded copies (web/assets.h).
 * MODE "fail" fails every later durable write; "fail-once" fails only the next
 * one (a recoverable storage fault); "recovered-settings" starts from a
 * deliberately unreadable settings record (defaults plus a diagnostic).
 * MODE "cleanup-fail-once" fails the first audio close during revocation.
 * MODE "pairing-proof=NNNN" adds a SYNTHETIC in-process Moonlight client that
 * completes the real Gen7 exchange with PIN NNNN after a web PIN entry;
 * SP5_SYNTHETIC_CLIENT_NAME overrides its untrusted display name. */
#include "pairing_client.h"
#include "platform.h"
#include "protocol/http.h"
#include "protocol/pairing_state.h"
#include "web/assets.h"
#include "web/diagnostics.h"
#include "web/metrics.h"
#include "web/server.h"
#include "web_service.h"
#include <errno.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    sp5_storage_ops durable;
    bool fail;
    unsigned fail_remaining;
} storage;
typedef struct {
    sp5_metrics_web metrics;
    sp5_diagnostics diagnostics;
    atomic_bool stop;
    const char *mode;
    sp5_authority *authority;
    size_t collected_ends;
} extensions;
static bool extension_route(void *context, const sp5_request *request, uint64_t now,
                            sp5_web_response *response)
{
    extensions *e = context;
    size_t count = sp5_authority_end_count(e->authority);
    while (e->collected_ends < count) {
        sp5_session_end_record record;
        if (sp5_authority_end_at(e->authority, e->collected_ends, &record).status != SP5_OK ||
            sp5_diagnostics_end(&e->diagnostics, record).status != SP5_OK)
            break;
        ++e->collected_ends;
    }
    return sp5_metrics_web_route(&e->metrics, request, now, response) ||
           sp5_diagnostics_route(&e->diagnostics, request, now, response);
}
static void extension_settings(void *context, bool enabled, uint64_t now)
{
    sp5_metrics_web_requested(&((extensions *)context)->metrics, enabled, now);
}
static bool extension_effective(void *context)
{
    return sp5_metrics_web_effective(&((extensions *)context)->metrics);
}
static volatile sig_atomic_t quitting;
static void quit(int value)
{
    (void)value;
    quitting = 1;
}
static uint64_t now_us(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return 0;
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}
static void *metrics_worker(void *context)
{
    extensions *e = context;
    uint64_t id = 0;
    while (!atomic_load(&e->stop)) {
        uint64_t now = now_us();
        (void)sp5_metrics_web_worker_step(&e->metrics, now);
        if (sp5_metrics_web_effective(&e->metrics)) {
            sp5_frame_record frame = {.frame_id = ++id,
                                      .observed_us = now,
                                      .duration_us = {1000, 2000, 2000, 500, 20000},
                                      .flags = SP5_FRAME_ALL_VALID};
            if (strcmp(e->mode, "missing") == 0)
                frame.flags &= ~(UINT64_C(1) << SP5_STAGE_ENCODE);
            if (strcmp(e->mode, "overflow") == 0)
                frame.duration_us[0] = 70000;
            sp5_input_record input = {id, now, 500, 0, SP5_INPUT_VALID | SP5_INPUT_CONFIRMED};
            (void)sp5_metrics_push_frame(e->metrics.engine, &frame);
            (void)sp5_metrics_push_input(e->metrics.engine, &input);
        }
        (void)sp5_metrics_count_stream(
            e->metrics.engine, (sp5_metrics_stream_event){SP5_FRAME_NETWORK_SUBMITTED, 1000, 1200});
        struct timespec delay = {.tv_nsec = 50000000};
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        }
    }
    return NULL;
}
static sp5_bytes text(const char *s)
{
    return (sp5_bytes){(const uint8_t *)s, strlen(s)};
}
static sp5_result read_file(void *p, sp5_store_file f, sp5_buffer *out)
{
    storage *s = p;
    return s->durable.read(s->durable.context, f, out);
}
static sp5_result write_file(void *p, sp5_store_file f, sp5_bytes bytes)
{
    storage *s = p;
    if (s->fail_remaining != 0) {
        --s->fail_remaining;
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    return s->fail ? sp5_failure(SP5_REASON_PLATFORM)
                   : s->durable.replace(s->durable.context, f, bytes);
}
static sp5_result generate(void *p, sp5_buffer *out)
{
    storage *s = p;
    return s->durable.generate_identity(s->durable.context, out);
}
static sp5_bytes certificate_span(sp5_bytes identity)
{
    const char marker[] = "-----BEGIN CERTIFICATE-----";
    for (size_t i = 0; i + sizeof(marker) - 1 <= identity.size; ++i)
        if (memcmp(identity.data + i, marker, sizeof(marker) - 1) == 0)
            return (sp5_bytes){identity.data + i, identity.size - i};
    return (sp5_bytes){0};
}
static sp5_optional_u64 wall_us(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_REALTIME, &t) != 0 || t.tv_sec < 0)
        return (sp5_optional_u64){0};
    return (sp5_optional_u64){true, (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000};
}
/* The served bytes are the build's embedded copies; refuse to start when they
 * differ from the checkout so a stale build cannot pass browser tests. */
static bool assets_current(const char *root)
{
    static uint8_t source[SP5_WEB_RESPONSE_LIMIT + 1];
    for (size_t i = 0; i < sp5_web_asset_count; ++i) {
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s%s", root, sp5_web_assets[i].path);
        if (n < 0 || (size_t)n >= sizeof(path))
            return false;
        FILE *file = fopen(path, "rb");
        if (file == NULL)
            return false;
        size_t size = fread(source, 1, sizeof(source), file);
        bool complete = feof(file) != 0 && ferror(file) == 0;
        if (fclose(file) != 0 || !complete || size != sp5_web_assets[i].size ||
            memcmp(source, sp5_web_assets[i].data, size) != 0) {
            char message[1200];
            int written =
                snprintf(message, sizeof(message), "embedded %s differs from %s; rebuild\n",
                         sp5_web_assets[i].path, path);
            if (written > 0 && (size_t)written < sizeof(message))
                (void)fputs(message, stderr);
            return false;
        }
    }
    return true;
}
/* Starts one real getservercert exchange from a SYNTHETIC client identity. */
static sp5_result begin_client(sp5_fake_pair_client *client, sp5_pair_flow *flow,
                               sp5_pairing *pairing, const sp5_storage_ops *storage, uint64_t now)
{
    uint8_t client_identity[SP5_IDENTITY_CAPACITY];
    sp5_buffer identity = {client_identity, sizeof(client_identity), 0};
    sp5_result result = storage->generate_identity(storage->context, &identity);
    if (result.status == SP5_OK)
        result = sp5_fake_pair_client_open(client, (sp5_bytes){identity.data, identity.size});
    OPENSSL_cleanse(client_identity, sizeof(client_identity));
    unsigned code = 0;
    bool deferred = false;
    /* Browser tests may supply hostile/long untrusted display metadata. */
    const char *name = getenv("SP5_SYNTHETIC_CLIENT_NAME");
    if (result.status == SP5_OK)
        result = sp5_fake_pair_client_begin(client, flow, pairing,
                                            text(name != NULL ? name : "SyntheticPhone"), now,
                                            &code, &deferred);
    return result.status == SP5_OK && (code != 200 || !deferred) ? sp5_failure(SP5_REASON_PLATFORM)
                                                                 : result;
}
int main(int argc, char **argv)
{
    if (argc != 4)
        return 64;
    const char *mode = argv[3];
    char proof_pin[5] = {0};
    if (strncmp(mode, "pairing-proof=", 14) == 0) {
        for (size_t i = 0; i < 4; ++i)
            if (mode[14 + i] < '0' || mode[14 + i] > '9')
                return 64;
        if (mode[18] != 0)
            return 64;
        memcpy(proof_pin, mode + 14, 4);
    }
    if (!assets_current(argv[2]))
        return 65;
    int result = 1;
    sp5_posix_storage *disk = NULL;
    sp5_store *store = NULL;
    sp5_authority *authority = NULL;
    sp5_session *session = NULL;
    sp5_fake_platform *fake = NULL;
    sp5_lifecycle *life = NULL;
    sp5_pairing *pairing = NULL;
    sp5_web *web = NULL;
    sp5_pair_flow *pair_flow = NULL;
    sp5_fake_web *service = NULL;
    sp5_fake_pair_client *client = NULL;
    extensions extension = {0};
    pthread_t metrics_thread;
    bool thread_started = false;
    if (signal(SIGTERM, quit) == SIG_ERR || signal(SIGINT, quit) == SIG_ERR ||
        signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        goto cleanup;
    disk = sp5_posix_storage_open(argv[1]);
    if (!disk)
        goto cleanup;
    storage io = {.durable = sp5_posix_storage_operations(disk)};
    sp5_storage_ops ops = {&io, read_file, write_file, generate};
    store = sp5_store_create(&ops);
    if (!store || sp5_store_load(store).status != SP5_OK)
        goto cleanup;
    /* Synthetic torn settings record, then a real reload through the store. */
    if (strcmp(mode, "recovered-settings") == 0 &&
        (io.durable.replace(io.durable.context, SP5_STORE_SETTINGS, text("unreadable")).status !=
             SP5_OK ||
         sp5_store_load(store).status != SP5_OK || !sp5_store_read(store).settings_recovered))
        goto cleanup;
    sp5_devices *devices = sp5_store_devices(store);
    if (!sp5_devices_count(devices) &&
        (sp5_devices_pair(devices, text("synthetic-A"),
                          text(strcmp(mode, "cleanup-fail-once") == 0
                                   ? "Tablet <img src=x onerror=alert(1)>"
                                   : "Tablet"),
                          1700000000000000)
                 .status != SP5_OK ||
         sp5_devices_pair(devices, text("synthetic-B"), text("Phone"), 1700000001000000).status !=
             SP5_OK))
        goto cleanup;
    sp5_host host;
    if (sp5_host_init(&host, "portable-fixture", "13.60", "192.168.50.1").status != SP5_OK ||
        sp5_host_refresh(&host, store).status != SP5_OK ||
        sp5_host_transition(&host, SP5_HOST_RUNNING).status != SP5_OK)
        goto cleanup;
    host.supported_mode_count = 1;
    host.supported_modes[0] = (sp5_host_mode){1280, 720, 60, "synthetic", SP5_SDR, 100, 1000};
    authority = sp5_authority_create(devices);
    session = sp5_session_create(authority);
    fake = calloc(1, sizeof(*fake));
    if (!session || !fake)
        goto cleanup;
    sp5_fake_init(fake);
    sp5_lifecycle_ops life_ops = {fake->capture, fake->encoder, fake->audio, fake->input,
                                  fake->system};
    life = sp5_lifecycle_create(1, &life_ops);
    sp5_lifecycle_policy policy = {.mode_count = 1,
                                   .media_bytes = 4096,
                                   .core_bytes = 4096,
                                   .streaming_cpu_us_per_10s = 100,
                                   .idle_cpu_us_per_10s = 1,
                                   .validity = SP5_VALIDITY_VALID,
                                   .synthetic = true,
                                   .evidence_ref = "browser:synthetic-only",
                                   .clock = fake->clock,
                                   .metrics_capacity = {1024, 1024, 64}};
    policy.modes[0] = host.supported_modes[0];
    policy.metrics_context.synthetic = true;
    uint64_t started = now_us();
    fake->now.ticks = started;
    (void)snprintf(policy.metrics_context.host_version, sizeof(policy.metrics_context.host_version),
                   "portable-fixture");
    (void)snprintf(policy.metrics_context.firmware_version,
                   sizeof(policy.metrics_context.firmware_version), "13.60 (synthetic)");
    (void)snprintf(policy.metrics_context.client_name, sizeof(policy.metrics_context.client_name),
                   "Tablet");
    policy.metrics_context.requested_bitrate_kbps = 5000;
    sp5_session_parameters parameters = {.requested_kbps = 5000, .started_us = started};
    sp5_media_config media = {1280, 720, 60, 1, "synthetic", SP5_SDR, 1000};
    if (!life || sp5_session_admit(session, text("synthetic-A"), 1).status != SP5_OK ||
        sp5_session_configure(session, &host, &parameters).status != SP5_OK ||
        sp5_lifecycle_configure(life, &policy).status != SP5_OK ||
        sp5_lifecycle_bind(life, session, text("synthetic-A")).status != SP5_OK ||
        sp5_lifecycle_start(life, &media, started).status != SP5_OK)
        goto cleanup;
    if (sp5_session_activate(session).status != SP5_OK)
        goto cleanup;
    pairing = sp5_pairing_create();
    if (!pairing)
        goto cleanup;
    if (strcmp(mode, "pairing") == 0) {
        uint64_t request_id;
        if (sp5_pairing_begin_request(pairing, now_us(), (sp5_optional_u64){0},
                                      text("Synthetic phone"), &request_id)
                .status != SP5_OK)
            goto cleanup;
    }
    service = calloc(1, sizeof(*service));
    client = calloc(1, sizeof(*client));
    if (service == NULL || client == NULL)
        goto cleanup;
    sp5_fake_web_init(service, NULL, -1, "192.168.50.2");
    char served[64];
    service->listener = sp5_fake_web_listen("127.0.0.1", 0, served, sizeof(served));
    service->truncate_streams = strcmp(mode, "truncate") == 0;
    if (service->listener < 0)
        goto cleanup;
    bool proof = proof_pin[0] != 0;
    if (strcmp(mode, "pairing-ready") == 0 || proof) {
        sp5_bytes identity = sp5_store_identity(store);
        sp5_pair_flow_config pairing_config = {.state = pairing,
                                               .devices = devices,
                                               .certificate_pem = certificate_span(identity),
                                               .private_key_pem = identity,
                                               .system = fake->system,
                                               .pin_url = "http://fixture.invalid/"};
        pair_flow = sp5_pair_flow_create(&pairing_config);
        if (pair_flow == NULL ||
            begin_client(client, pair_flow, pairing, &ops, now_us()).status != SP5_OK)
            goto cleanup;
    }
    extension = (extensions){.mode = mode, .authority = authority};
    atomic_init(&extension.stop, false);
    sp5_metrics *metrics = sp5_lifecycle_metrics(life);
    if (sp5_metrics_web_init(&extension.metrics, metrics, sp5_store_settings(store).metrics_enabled,
                             started)
                .status != SP5_OK ||
        sp5_diagnostics_init(&extension.diagnostics, metrics, "portable-fixture",
                             "13.60 (synthetic)")
                .status != SP5_OK)
        goto cleanup;
    if (sp5_diagnostics_log(&extension.diagnostics, started, 1, SP5_LOG_DIAGNOSTIC, SP5_REASON_NONE)
            .status != SP5_OK)
        goto cleanup;
    /* FR-039: the store's load diagnostic reaches the downloadable bundle once. */
    sp5_store_view loaded = sp5_store_read(store);
    if ((loaded.settings_recovered || loaded.repairing_required) &&
        sp5_diagnostics_log(&extension.diagnostics, started, 0, SP5_LOG_DIAGNOSTIC,
                            loaded.diagnostic)
                .status != SP5_OK)
        goto cleanup;
    if (pthread_create(&metrics_thread, NULL, metrics_worker, &extension) != 0)
        goto cleanup;
    thread_started = true;
    sp5_web_config config = {
        .store = store,
        .host = &host,
        .session = session,
        .lifecycle = life,
        .pairing = pairing,
        .pair_flow = pair_flow,
        .authority = served,
        .extensions = {&extension, extension_route, extension_settings, extension_effective}};
    web = sp5_web_create(&config);
    if (!web)
        goto cleanup;
    service->web = web;
    io.fail = strcmp(mode, "fail") == 0;
    io.fail_remaining = strcmp(mode, "fail-once") == 0 ? 1 : 0;
    if (strcmp(mode, "cleanup-fail-once") == 0)
        fake->fail_next[SP5_FAKE_AUDIO_CLOSE] = SP5_REASON_PLATFORM;
    if (printf("http://%s\n", served) < 0 || fflush(stdout) != 0)
        goto cleanup;
    result = 0;
    while (!quitting && !service->stopped) {
        struct pollfd wait[SP5_FAKE_WEB_POLLS];
        sp5_fake_web_poll(service, wait);
        if (poll(wait, SP5_FAKE_WEB_POLLS, 50) < 0) {
            if (errno == EINTR)
                continue;
            result = 1;
            break;
        }
        uint64_t now = now_us();
        sp5_fake_web_step(service, wait, now, false);
        if (pair_flow != NULL && sp5_pair_flow_tick(pair_flow, now).status != SP5_OK) {
            result = 1;
            break;
        }
        /* SYNTHETIC client: proves with its own PIN once the web page entered one. */
        if (proof && client->request_id != 0) {
            bool paired = false;
            sp5_result proved = sp5_fake_pair_client_prove(
                client, pair_flow, (sp5_bytes){(const uint8_t *)proof_pin, 4}, now, wall_us(),
                &paired);
            if (proved.status != SP5_OK && proved.reason != SP5_REASON_WOULD_BLOCK) {
                result = 1;
                break;
            }
        }
    }
cleanup:
    if (thread_started) {
        atomic_store(&extension.stop, true);
        if (pthread_join(metrics_thread, NULL) != 0)
            result = 1;
    }
    if (service != NULL) {
        sp5_fake_web_close(service);
        if (service->failed)
            result = 1;
    }
    if (life != NULL && sp5_lifecycle_end(life, SP5_END_HOST_STOPPING).status != SP5_OK)
        result = 1;
    sp5_web_destroy(web);
    sp5_pair_flow_destroy(pair_flow);
    sp5_pairing_destroy(pairing);
    sp5_lifecycle_destroy(life);
    free(fake);
    sp5_fake_pair_client_close(client);
    free(client);
    free(service);
    sp5_session_destroy(session);
    sp5_authority_destroy(authority);
    sp5_store_destroy(store);
    if (disk != NULL && sp5_posix_storage_close(disk).status != SP5_OK)
        result = 1;
    return result;
}
