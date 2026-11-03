/* T109 synthetic harness: HTTP/HTTPS framing, GameStream routes, Gen7 pairing
 * bodies and the management web dispatch (headers, page token, JSON fields).
 * No socket, console adapter, official client or reference host is involved;
 * every identity is generated in memory and never leaves the process.
 *
 * Raw input: one request through every boundary with a fresh state.
 * "SP5R" input: up to RECORD_LIMIT records [u8 kind][u16 length LE][payload]
 * sharing one fresh pairing/web state, so getservercert -> web PIN ->
 * certificate release -> challenge steps -> /unpair sequences are reachable.
 * In web payloads "@TOKEN@" becomes the page token and "@RID@" the pending
 * pairing request id; both are random per run and otherwise unguessable. */
#include "protocol/http.h"
#include "harness.h"
#include "pairing.h"
#include "protocol/pairing_flow.h"
#include "web/json.h"
#include "web/server.h"
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <string.h>

enum {
    INPUT_LIMIT = 32768,
    RECORD_LIMIT = 32,
    WIRE_LIMIT = SP5_HTTP_LIMIT + 1,
    EXPANDED_LIMIT = SP5_HTTP_LIMIT * 2,
    KEY_LIMIT = 4096,
    FILE_LIMIT = 8192
};
typedef enum {
    RECORD_WEB = 0,
    RECORD_HTTP,
    RECORD_HTTPS,
    RECORD_PAIR_DIRECT,
    RECORD_TAKE_CERTIFICATE,
    RECORD_ADVANCE,
    RECORD_BEGIN_PAIRING,
    RECORD_KIND_COUNT
} record_kind;

typedef struct {
    uint8_t cert[SP5_CERTIFICATE_CAPACITY], key[KEY_LIMIT];
    size_t cert_size, key_size;
} identity;
typedef struct {
    sp5_pair_flow *flow;
} pair_holder;
/* Process-lifetime fixture: the TLS session must belong to this service. */
typedef struct {
    identity server, client;
    sp5_devices *devices;
    sp5_authority *authority;
    sp5_session *session;
    sp5_host host;
    sp5_http_service *service;
    pair_holder holder;
    SSL_CTX *client_context;
    SSL *tls, *peer;
} persistent;
typedef struct {
    uint8_t data[SP5_STORE_FILE_COUNT][FILE_LIMIT];
    size_t size[SP5_STORE_FILE_COUNT];
} memory_files;
/* Per-input state: recreated so every execution is deterministic. */
typedef struct {
    memory_files files;
    sp5_store *store;
    sp5_host host;
    sp5_authority *authority;
    sp5_session *session;
    sp5_devices *flow_devices;
    sp5_pairing *pairing;
    sp5_pair_flow *flow;
    sp5_web *web;
    char token[65];
    uint64_t now, pending_id;
    bool deferred;
} run_state;

static sp5_bytes text(const char *value)
{
    return (sp5_bytes){(const uint8_t *)value, strlen(value)};
}
static sp5_result memory_read(void *context, sp5_store_file file, sp5_buffer *output)
{
    memory_files *files = context;
    return files->size[file] == 0
               ? sp5_failure(SP5_REASON_WOULD_BLOCK)
               : sp5_buffer_append(output, (sp5_bytes){files->data[file], files->size[file]});
}
static sp5_result memory_replace(void *context, sp5_store_file file, sp5_bytes contents)
{
    memory_files *files = context;
    if (contents.size > FILE_LIMIT) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    memcpy(files->data[file], contents.data, contents.size);
    files->size[file] = contents.size;
    return sp5_success();
}
static sp5_result memory_identity(void *context, sp5_buffer *output)
{
    (void)context;
    /* Opaque web-store identity bytes; never a key and never used for TLS. */
    return sp5_buffer_append(output, text("synthetic-fuzz-store-identity"));
}
static sp5_result notify(void *context, const char *message, size_t size)
{
    (void)context;
    fuzz_require(message != NULL && size > 0);
    return sp5_success();
}

static void make_identity(identity *value, const char *name)
{
    EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", 2048);
    X509 *cert = X509_new();
    fuzz_require(key != NULL && cert != NULL);
    fuzz_require(X509_set_version(cert, 2) == 1);
    fuzz_require(ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) == 1);
    fuzz_require(X509_gmtime_adj(X509_getm_notBefore(cert), -60) != NULL);
    fuzz_require(X509_gmtime_adj(X509_getm_notAfter(cert), 86400L * 3650) != NULL);
    fuzz_require(X509_set_pubkey(cert, key) == 1);
    X509_NAME *subject = X509_get_subject_name(cert);
    fuzz_require(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                            (const unsigned char *)name, -1, -1, 0) == 1);
    fuzz_require(X509_set_issuer_name(cert, subject) == 1);
    fuzz_require(X509_sign(cert, key, EVP_sha256()) > 0);
    BIO *cert_bio = BIO_new(BIO_s_mem());
    BIO *key_bio = BIO_new(BIO_s_mem());
    fuzz_require(cert_bio != NULL && key_bio != NULL);
    fuzz_require(PEM_write_bio_X509(cert_bio, cert) == 1);
    fuzz_require(PEM_write_bio_PrivateKey(key_bio, key, NULL, NULL, 0, NULL, NULL) == 1);
    char *cert_data = NULL;
    char *key_data = NULL;
    long cert_size = BIO_get_mem_data(cert_bio, &cert_data);
    long key_size = BIO_get_mem_data(key_bio, &key_data);
    fuzz_require(cert_size > 0 && (size_t)cert_size <= sizeof(value->cert));
    fuzz_require(key_size > 0 && (size_t)key_size <= sizeof(value->key));
    memcpy(value->cert, cert_data, (size_t)cert_size);
    memcpy(value->key, key_data, (size_t)key_size);
    value->cert_size = (size_t)cert_size;
    value->key_size = (size_t)key_size;
    BIO_free(cert_bio);
    BIO_free(key_bio);
    X509_free(cert);
    EVP_PKEY_free(key);
}

static sp5_result pair_trampoline(void *context, const sp5_request *request,
                                  sp5_bytes verified_certificate, uint64_t now_us,
                                  sp5_optional_u64 wall_us, sp5_http_response *response)
{
    const pair_holder *holder = context;
    if (holder->flow == NULL) {
        response->code = 503;
        return sp5_success();
    }
    return sp5_pair_flow_handle(holder->flow, request, verified_certificate, now_us, wall_us,
                                response);
}

/* ends[0] is the host side, ends[1] the client side of one in-memory BIO pair. */
static bool handshake(SSL *const ends[2])
{
    SSL *server = ends[0];
    SSL *client = ends[1];
    for (unsigned i = 0; i < 30; ++i) {
        int result = SSL_do_handshake(client);
        int error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(client, result);
        if (error != SSL_ERROR_NONE && error != SSL_ERROR_WANT_READ &&
            error != SSL_ERROR_WANT_WRITE) {
            return false;
        }
        result = SSL_do_handshake(server);
        error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(server, result);
        if (error != SSL_ERROR_NONE && error != SSL_ERROR_WANT_READ &&
            error != SSL_ERROR_WANT_WRITE) {
            return false;
        }
        if (SSL_is_init_finished(server) && SSL_is_init_finished(client)) {
            return true;
        }
    }
    return false;
}

static persistent *setup(void)
{
    static persistent fixture;
    static memory_files files;
    make_identity(&fixture.server, "Synthetic fuzz host");
    make_identity(&fixture.client, "Synthetic fuzz client");
    sp5_storage_ops ops = {&files, memory_read, memory_replace, memory_identity};
    fixture.devices = sp5_devices_create(&ops);
    fuzz_require(fixture.devices != NULL && sp5_devices_load(fixture.devices).status == SP5_OK);
    sp5_bytes client = {fixture.client.cert, fixture.client.cert_size};
    fuzz_require(
        sp5_devices_pair(fixture.devices, client, text("Fuzz client"), UINT64_C(1761800000000000))
            .status == SP5_OK);
    fixture.authority = sp5_authority_create(fixture.devices);
    fixture.session = fixture.authority ? sp5_session_create(fixture.authority) : NULL;
    fuzz_require(fixture.session != NULL);
    fuzz_require(sp5_host_init(&fixture.host, "fuzz-host", "13.60", "10.0.0.2").status == SP5_OK);
    fuzz_require(sp5_host_transition(&fixture.host, SP5_HOST_RUNNING).status == SP5_OK);
    /* Labeled fixture modes only; never a console capability claim. */
    fixture.host.supported_mode_count = 2;
    fixture.host.supported_modes[0] = (sp5_host_mode){1280, 720, 60, "h264", SP5_SDR, 100, 1000};
    fixture.host.supported_modes[1] =
        (sp5_host_mode){1920, 1080, 60, "hevc", SP5_HDR_PQ_BT2020, 200, 2000};
    sp5_http_config config = {.host = &fixture.host,
                              .devices = fixture.devices,
                              .session = fixture.session,
                              .certificate_pem = {fixture.server.cert, fixture.server.cert_size},
                              .private_key_pem = {fixture.server.key, fixture.server.key_size},
                              .pair_context = &fixture.holder,
                              .pair = pair_trampoline};
    fixture.service = sp5_http_create(&config);
    fuzz_require(fixture.service != NULL);
    /* One in-memory TLS handshake with the paired client identity. */
    fixture.client_context = SSL_CTX_new(TLS_client_method());
    fuzz_require(fixture.client_context != NULL);
    BIO *cert_bio = BIO_new_mem_buf(fixture.client.cert, (int)fixture.client.cert_size);
    BIO *key_bio = BIO_new_mem_buf(fixture.client.key, (int)fixture.client.key_size);
    X509 *cert = cert_bio ? PEM_read_bio_X509(cert_bio, NULL, NULL, NULL) : NULL;
    EVP_PKEY *key = key_bio ? PEM_read_bio_PrivateKey(key_bio, NULL, NULL, NULL) : NULL;
    fuzz_require(cert != NULL && key != NULL);
    fuzz_require(SSL_CTX_use_certificate(fixture.client_context, cert) == 1);
    fuzz_require(SSL_CTX_use_PrivateKey(fixture.client_context, key) == 1);
    SSL_CTX_set_verify(fixture.client_context, SSL_VERIFY_NONE, NULL);
    X509_free(cert);
    EVP_PKEY_free(key);
    BIO_free(cert_bio);
    BIO_free(key_bio);
    fixture.tls = SSL_new(sp5_http_tls_context(fixture.service));
    fixture.peer = SSL_new(fixture.client_context);
    BIO *server_io = NULL;
    BIO *client_io = NULL;
    fuzz_require(fixture.tls != NULL && fixture.peer != NULL &&
                 BIO_new_bio_pair(&server_io, 0, &client_io, 0) == 1);
    SSL_set_bio(fixture.tls, server_io, server_io);
    SSL_set_bio(fixture.peer, client_io, client_io);
    SSL_set_accept_state(fixture.tls);
    SSL_set_connect_state(fixture.peer);
    SSL *const ends[2] = {fixture.tls, fixture.peer};
    fuzz_require(handshake(ends));
    return &fixture;
}

static uint64_t wall(const run_state *run)
{
    return UINT64_C(1761800000000000) + run->now;
}

static void begin_run(persistent *fixture, run_state *run)
{
    memset(run, 0, sizeof(*run));
    run->now = UINT64_C(1000000);
    sp5_storage_ops ops = {&run->files, memory_read, memory_replace, memory_identity};
    run->store = sp5_store_create(&ops);
    fuzz_require(run->store != NULL && sp5_store_load(run->store).status == SP5_OK);
    sp5_devices *devices = sp5_store_devices(run->store);
    /* Fixed ids: lowercase SHA-256 of "A" and "B" (seeds address them). */
    fuzz_require(sp5_devices_pair(devices, text("A"), text("Tablet"), 100).status == SP5_OK);
    fuzz_require(sp5_devices_pair(devices, text("B"), text("Phone"), 101).status == SP5_OK);
    fuzz_require(sp5_host_init(&run->host, "fuzz-web", "13.60", "10.0.0.2").status == SP5_OK);
    fuzz_require(sp5_host_transition(&run->host, SP5_HOST_RUNNING).status == SP5_OK);
    run->host.supported_mode_count = 1;
    run->host.supported_modes[0] = (sp5_host_mode){1280, 720, 60, "h264", SP5_SDR, 100, 1000};
    run->authority = sp5_authority_create(devices);
    run->session = run->authority ? sp5_session_create(run->authority) : NULL;
    fuzz_require(run->session != NULL);
    sp5_session_parameters parameters = {.requested_kbps = 5000, .started_us = run->now};
    fuzz_require(sp5_session_admit(run->session, text("A"), 1).status == SP5_OK &&
                 sp5_session_configure(run->session, &run->host, &parameters).status == SP5_OK &&
                 sp5_session_activate(run->session).status == SP5_OK);
    run->flow_devices = sp5_devices_create(
        &(sp5_storage_ops){&run->files, memory_read, memory_replace, memory_identity});
    fuzz_require(run->flow_devices != NULL);
    /* Separate registry: pairing proofs never change the TLS fixture registry. */
    fuzz_require(sp5_devices_pair(run->flow_devices,
                                  (sp5_bytes){fixture->client.cert, fixture->client.cert_size},
                                  text("Fuzz client"), 100)
                     .status == SP5_OK);
    run->pairing = sp5_pairing_create();
    fuzz_require(run->pairing != NULL);
    sp5_pair_flow_config pair = {
        .state = run->pairing,
        .devices = run->flow_devices,
        .certificate_pem = {fixture->server.cert, fixture->server.cert_size},
        .private_key_pem = {fixture->server.key, fixture->server.key_size},
        .system = {.notify = notify},
        .pin_url = "http://10.0.0.2:47990"};
    run->flow = sp5_pair_flow_create(&pair);
    fuzz_require(run->flow != NULL);
    fixture->holder.flow = run->flow;
    sp5_web_config web = {.store = run->store,
                          .host = &run->host,
                          .session = run->session,
                          .pairing = run->pairing,
                          .pair_flow = run->flow,
                          .authority = "10.0.0.2:47990"};
    run->web = sp5_web_create(&web);
    fuzz_require(run->web != NULL);
    static uint8_t body[SP5_WEB_RESPONSE_LIMIT];
    sp5_web_response response = {.body = {body, sizeof(body), 0}};
    const char page[] = "GET /api/v1/page HTTP/1.1\r\nHost: 10.0.0.2:47990\r\n\r\n";
    fuzz_require(
        sp5_web_dispatch(run->web, "10.1.2.3", false, text(page), ++run->now, &response).status ==
            SP5_OK &&
        response.status == 200 && response.body.size > 74);
    const char *token = memchr(body, ':', response.body.size);
    token = token ? memchr(token + 1, ':', response.body.size - (size_t)(token + 1 - (char *)body))
                  : NULL;
    fuzz_require(token != NULL && token[1] == '"');
    memcpy(run->token, token + 2, 64);
    run->token[64] = 0;
}

static void end_run(persistent *fixture, run_state *run)
{
    /* Reset the process-lifetime service: /cancel ends its session and launch. */
    uint8_t fields[SP5_HTTP_LIMIT];
    sp5_http_response response = {.fields = {fields, sizeof(fields), 0}};
    sp5_request cancel = {.method = text("GET"), .target = text("/cancel")};
    fuzz_require(sp5_http_route(fixture->service, "10.1.2.3", false, fixture->tls, &cancel,
                                run->now, (sp5_optional_u64){0}, &response)
                     .status == SP5_OK);
    fuzz_require(sp5_session_read(fixture->session).state == SP5_SESSION_ENDED ||
                 sp5_session_read(fixture->session).id == 0);
    fixture->holder.flow = NULL;
    sp5_web_destroy(run->web);
    sp5_pair_flow_destroy(run->flow);
    sp5_pairing_destroy(run->pairing);
    sp5_devices_destroy(run->flow_devices);
    sp5_session_destroy(run->session);
    sp5_authority_destroy(run->authority);
    sp5_store_destroy(run->store);
}

/* Replace one placeholder everywhere, bounded by the output capacity. */
static sp5_bytes substitute(sp5_bytes input, const char *pattern, const char *value,
                            uint8_t *output, size_t capacity)
{
    size_t pattern_size = strlen(pattern);
    size_t value_size = strlen(value);
    size_t used = 0;
    for (size_t i = 0; i < input.size;) {
        bool match =
            input.size - i >= pattern_size && memcmp(input.data + i, pattern, pattern_size) == 0;
        size_t length = match ? value_size : 1;
        if (used + length > capacity) {
            break;
        }
        if (match) {
            memcpy(output + used, value, length);
            i += pattern_size;
        } else {
            output[used] = input.data[i++];
        }
        used += length;
    }
    return (sp5_bytes){output, used};
}

/* "@TOKEN@" and "@RID@" expand first; "@LEN@" then becomes the length of the
 * expanded body after the first blank line, so seeds keep a matching
 * Content-Length while the fuzzer remains free to write any other value. */
static sp5_bytes expand(const run_state *run, sp5_bytes input, uint8_t *output, size_t capacity)
{
    static uint8_t first[EXPANDED_LIMIT], second[EXPANDED_LIMIT];
    char request_id[24];
    int id_size = snprintf(request_id, sizeof(request_id), "%llu",
                           (unsigned long long)sp5_pairing_read(run->pairing, run->now).request_id);
    fuzz_require(id_size > 0 && (size_t)id_size < sizeof(request_id));
    sp5_bytes wire = substitute(input, "@TOKEN@", run->token, first, sizeof(first));
    wire = substitute(wire, "@RID@", request_id, second, sizeof(second));
    size_t body = wire.size;
    for (size_t i = 0; i + 4 <= wire.size; ++i) {
        if (memcmp(wire.data + i, "\r\n\r\n", 4) == 0) {
            body = wire.size - i - 4;
            break;
        }
    }
    char length[24];
    int length_size = snprintf(length, sizeof(length), "%zu", body);
    fuzz_require(length_size > 0 && (size_t)length_size < sizeof(length));
    return substitute(wire, "@LEN@", length, output, capacity);
}

static void web(run_state *run, sp5_bytes input)
{
    static uint8_t expanded[EXPANDED_LIMIT];
    static uint8_t body[SP5_WEB_RESPONSE_LIMIT];
    sp5_bytes wire = expand(run, input, expanded, sizeof(expanded));
    sp5_web_response response = {.body = {body, sizeof(body), 0}};
    sp5_result result = sp5_web_dispatch(run->web, "10.1.2.3", false, wire, ++run->now, &response);
    fuzz_require(response.body.size <= response.body.capacity);
    if (result.status == SP5_OK) {
        fuzz_require(response.status >= 200 && response.status <= 599);
        fuzz_require(response.content_type != NULL);
    }
    if (response.stopping) {
        fuzz_require(sp5_web_finish_stop(run->web).status == SP5_OK);
    }
}

static void render(const sp5_http_response *response)
{
    static uint8_t wire[SP5_HTTP_LIMIT + 512];
    sp5_buffer output = {wire, sizeof(wire), 0};
    fuzz_require(response->fields.size <= response->fields.capacity);
    if (!response->deferred) {
        (void)sp5_http_render(response, &output);
        fuzz_require(output.size <= output.capacity);
    }
}

static void route(persistent *fixture, run_state *run, const sp5_request *request, SSL *tls)
{
    static uint8_t fields[SP5_HTTP_LIMIT];
    sp5_http_response response = {.fields = {fields, sizeof(fields), 0}};
    ++run->now;
    sp5_result result = sp5_http_route(fixture->service, "10.1.2.3", false, tls, request, run->now,
                                       (sp5_optional_u64){true, wall(run)}, &response);
    if (result.status == SP5_OK) {
        fuzz_require(response.code >= 100 && response.code <= 599);
        render(&response);
        if (response.deferred) {
            run->deferred = true;
            run->pending_id = sp5_pairing_read(run->pairing, run->now).request_id;
        }
    }
    sp5_http_launch launch;
    if (sp5_http_launch_read(fixture->service, &launch).status == SP5_OK) {
        fuzz_require(launch.session_id != 0);
    }
}

static void gamestream(persistent *fixture, run_state *run, sp5_bytes wire, SSL *tls)
{
    sp5_request request;
    if (sp5_http_parse(wire, SP5_HTTP_LIMIT, &request).status == SP5_OK) {
        route(fixture, run, &request, tls);
    }
}

static void pair_direct(persistent *fixture, run_state *run, sp5_bytes wire)
{
    sp5_request request;
    if (sp5_http_parse(wire, SP5_HTTP_LIMIT, &request).status != SP5_OK) {
        return;
    }
    static uint8_t fields[SP5_HTTP_LIMIT];
    sp5_http_response response = {.fields = {fields, sizeof(fields), 0}};
    /* Synthetic stand-in for a TLS-verified certificate (pairchallenge path). */
    sp5_bytes verified = {fixture->client.cert, fixture->client.cert_size};
    ++run->now;
    if (sp5_pair_flow_handle(run->flow, &request, verified, run->now,
                             (sp5_optional_u64){true, wall(run)}, &response)
            .status == SP5_OK) {
        render(&response);
        if (response.deferred) {
            run->deferred = true;
            run->pending_id = sp5_pairing_read(run->pairing, run->now).request_id;
        }
    }
}

static void take_certificate(run_state *run)
{
    if (!run->deferred) {
        return;
    }
    static uint8_t fields[SP5_HTTP_LIMIT];
    sp5_http_response response = {.fields = {fields, sizeof(fields), 0}};
    sp5_result result =
        sp5_pair_flow_take_certificate(run->flow, run->pending_id, ++run->now, &response);
    if (result.status == SP5_OK) {
        render(&response);
        run->deferred = false;
    } else if (result.reason != SP5_REASON_WOULD_BLOCK) {
        run->deferred = false;
    }
}

static void advance(run_state *run, sp5_bytes payload)
{
    uint32_t delta = 0;
    for (size_t i = 0; i < payload.size && i < 4; ++i) {
        delta |= (uint32_t)payload.data[i] << (8 * i);
    }
    /* Up to about 71 minutes per record: reaches 5-minute expiry and cooldown. */
    run->now += delta;
    fuzz_require(sp5_pair_flow_tick(run->flow, run->now).status == SP5_OK);
}

/* Moonlight getservercert with the harness client certificate appended. */
static void begin_pairing(persistent *fixture, run_state *run, sp5_bytes payload)
{
    static char wire[SP5_HTTP_LIMIT];
    static const char hex[] = "0123456789abcdef";
    int prefix = snprintf(wire, sizeof(wire), "GET /pair?%.*s&clientcert=", (int)payload.size,
                          (const char *)payload.data);
    if (prefix < 0 || (size_t)prefix >= sizeof(wire) ||
        (size_t)prefix + fixture->client.cert_size * 2 + 32 > sizeof(wire)) {
        return;
    }
    size_t used = (size_t)prefix;
    for (size_t i = 0; i < fixture->client.cert_size; ++i) {
        wire[used++] = hex[fixture->client.cert[i] >> 4];
        wire[used++] = hex[fixture->client.cert[i] & 15];
    }
    const char suffix[] = " HTTP/1.1\r\n\r\n";
    memcpy(wire + used, suffix, sizeof(suffix) - 1);
    used += sizeof(suffix) - 1;
    gamestream(fixture, run, (sp5_bytes){(const uint8_t *)wire, used}, NULL);
}

static void stateless(sp5_bytes wire)
{
    sp5_request request;
    sp5_json_object object;
    /* HTTP and decrypted HTTPS share exactly this framing/parser boundary. */
    if (sp5_http_parse(wire, SP5_HTTP_LIMIT, &request).status == SP5_OK) {
        fuzz_span(wire, request.method);
        fuzz_span(wire, request.target);
        fuzz_span(wire, request.body);
        const char *const keys[] = {"salt",
                                    "clientcert",
                                    "clientchallenge",
                                    "serverchallengeresp",
                                    "clientpairingsecret",
                                    "uniqueid",
                                    "phrase",
                                    "rikey",
                                    "appid",
                                    "mode"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            uint8_t decoded[2048];
            sp5_buffer output = {decoded, sizeof(decoded), 0};
            bool present = false;
            (void)sp5_http_query(&request, keys[i], &output, &present);
            fuzz_require(output.size <= output.capacity);
        }
        (void)sp5_json_parse(request.body, &object);
    }
    /* Covers management/pairing JSON even before the framing becomes valid. */
    (void)sp5_json_parse(wire, &object);
    uint8_t key[16];
    sp5_buffer output = {key, sizeof(key), 0};
    size_t salt_size = wire.size < 16 ? wire.size : 16;
    (void)sp5_pairing_pin_key((sp5_bytes){wire.data, salt_size},
                              (sp5_bytes){wire.data + salt_size, wire.size - salt_size}, &output);
    fuzz_require(output.size <= output.capacity);
}

static void record(persistent *fixture, run_state *run, record_kind kind, sp5_bytes payload)
{
    switch (kind) {
    case RECORD_WEB:
        web(run, payload);
        break;
    case RECORD_HTTP:
        gamestream(fixture, run, payload, NULL);
        break;
    case RECORD_HTTPS:
        gamestream(fixture, run, payload, fixture->tls);
        break;
    case RECORD_PAIR_DIRECT:
        pair_direct(fixture, run, payload);
        break;
    case RECORD_TAKE_CERTIFICATE:
        take_certificate(run);
        break;
    case RECORD_ADVANCE:
        advance(run, payload);
        break;
    case RECORD_BEGIN_PAIRING:
        begin_pairing(fixture, run, payload);
        break;
    default:
        break;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static persistent *fixture;
    if (size > INPUT_LIMIT) {
        return 0;
    }
    if (fixture == NULL) {
        fixture = setup();
    }
    sp5_bytes input = {data, size};
    static run_state run;
    begin_run(fixture, &run);
    if (size >= 4 && memcmp(data, "SP5R", 4) == 0) {
        size_t at = 4;
        for (unsigned count = 0; count < RECORD_LIMIT && size - at >= 3; ++count) {
            record_kind kind = (record_kind)(data[at] % RECORD_KIND_COUNT);
            size_t length = (size_t)data[at + 1] | ((size_t)data[at + 2] << 8);
            at += 3;
            if (length > size - at) {
                length = size - at;
            }
            sp5_bytes payload = {data + at, length};
            fuzz_span(input, payload);
            if (length <= WIRE_LIMIT) {
                record(fixture, &run, kind, payload);
            }
            at += length;
        }
    } else if (size <= WIRE_LIMIT) {
        stateless(input);
        web(&run, input);
        gamestream(fixture, &run, input, NULL);
        gamestream(fixture, &run, input, fixture->tls);
        pair_direct(fixture, &run, input);
        take_certificate(&run);
    }
    end_run(fixture, &run);
    return 0;
}
