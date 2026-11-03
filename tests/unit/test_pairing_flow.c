/* Synthetic complete Gen7 exchange; no official client or console evidence. */
#include "protocol/pairing_flow.h"

#include <openssl/pem.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            if (fputs(__FILE__ ": " #condition "\n", stderr) == EOF) {                             \
                return 2;                                                                          \
            }                                                                                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static sp5_bytes bytes(const char *value)
{
    return (sp5_bytes){(const uint8_t *)value, strlen(value)};
}

typedef struct {
    uint8_t cert[SP5_CERTIFICATE_CAPACITY], key[4096];
    size_t cert_size, key_size;
    EVP_PKEY *private_key;
    X509 *certificate;
} identity;

static int make_identity(identity *value)
{
    value->private_key = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", 2048);
    value->certificate = X509_new();
    CHECK(value->private_key != NULL && value->certificate != NULL);
    CHECK(X509_set_version(value->certificate, 2) == 1);
    CHECK(ASN1_INTEGER_set(X509_get_serialNumber(value->certificate), 1) == 1);
    CHECK(X509_gmtime_adj(X509_getm_notBefore(value->certificate), -60) != NULL);
    CHECK(X509_gmtime_adj(X509_getm_notAfter(value->certificate), 3600) != NULL);
    CHECK(X509_set_pubkey(value->certificate, value->private_key) == 1);
    X509_NAME *name = X509_get_subject_name(value->certificate);
    CHECK(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                     (const unsigned char *)"Synthetic pairing", -1, -1, 0) == 1);
    CHECK(X509_set_issuer_name(value->certificate, name) == 1);
    CHECK(X509_sign(value->certificate, value->private_key, EVP_sha256()) > 0);
    BIO *cert = BIO_new(BIO_s_mem());
    BIO *key = BIO_new(BIO_s_mem());
    CHECK(cert != NULL && key != NULL);
    CHECK(PEM_write_bio_X509(cert, value->certificate) == 1);
    CHECK(PEM_write_bio_PrivateKey(key, value->private_key, NULL, NULL, 0, NULL, NULL) == 1);
    char *cert_data = NULL, *key_data = NULL;
    long cert_size = BIO_get_mem_data(cert, &cert_data);
    long key_size = BIO_get_mem_data(key, &key_data);
    CHECK(cert_size > 0 && (size_t)cert_size <= sizeof(value->cert));
    CHECK(key_size > 0 && (size_t)key_size <= sizeof(value->key));
    value->cert_size = (size_t)cert_size;
    value->key_size = (size_t)key_size;
    memcpy(value->cert, cert_data, value->cert_size);
    memcpy(value->key, key_data, value->key_size);
    BIO_free(cert);
    BIO_free(key);
    return 0;
}

typedef struct {
    uint8_t data[40000];
    size_t size;
    unsigned notifications;
    bool fail_write, fail_notify;
    char last_notification[256];
} files;

static sp5_result read_file(void *context, sp5_store_file file, sp5_buffer *output)
{
    files *value = context;
    if (file != SP5_STORE_DEVICES || value->size == 0) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    return sp5_buffer_append(output, (sp5_bytes){value->data, value->size});
}

static sp5_result write_file(void *context, sp5_store_file file, sp5_bytes data)
{
    files *value = context;
    if (value->fail_write) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    if (file != SP5_STORE_DEVICES || data.size > sizeof(value->data)) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    memcpy(value->data, data.data, data.size);
    value->size = data.size;
    return sp5_success();
}

static sp5_result notify(void *context, const char *message, size_t size)
{
    files *value = context;
    if (value->fail_notify) {
        return sp5_failure(SP5_REASON_PLATFORM);
    }
    if (size >= sizeof(value->last_notification)) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    memcpy(value->last_notification, message, size);
    value->last_notification[size] = 0;
    ++value->notifications;
    return sp5_success();
}

static int request(sp5_pair_flow *flow, sp5_http_response *response, const char *key,
                   sp5_bytes value, uint64_t now_us)
{
    char target[8192];
    static const char hex[] = "0123456789abcdef";
    int prefix_size = snprintf(target, sizeof(target), "/pair?uniqueid=fixture&%s=", key);
    CHECK(prefix_size > 0 && (size_t)prefix_size + value.size * 2 < sizeof(target));
    size_t offset = (size_t)prefix_size;
    for (size_t i = 0; i < value.size; ++i) {
        target[offset++] = hex[value.data[i] >> 4];
        target[offset++] = hex[value.data[i] & 15];
    }
    sp5_request input = {.method = bytes("GET"), .target = {(const uint8_t *)target, offset}};
    response->fields.size = 0;
    response->code = 200;
    response->deferred = false;
    CHECK(sp5_pair_flow_handle(flow, &input, (sp5_bytes){0}, now_us,
                               (sp5_optional_u64){.present = true, .value = 1234}, response)
              .status == SP5_OK);
    return 0;
}

static int start(sp5_pair_flow *flow, sp5_pairing *state, identity *client,
                 sp5_http_response *response, uint64_t now_us, uint64_t *request_id)
{
    char target[8192];
    const char prefix[] = "/pair?uniqueid=fixture&devicename=roth&phrase=getservercert&"
                          "salt=000102030405060708090a0b0c0d0e0f&clientcert=";
    memcpy(target, prefix, sizeof(prefix) - 1);
    size_t offset = sizeof(prefix) - 1;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < client->cert_size; ++i) {
        target[offset++] = hex[client->cert[i] >> 4];
        target[offset++] = hex[client->cert[i] & 15];
    }
    sp5_request input = {.method = bytes("GET"), .target = {(const uint8_t *)target, offset}};
    response->fields.size = 0;
    response->code = 200;
    response->deferred = false;
    CHECK(sp5_pair_flow_handle(flow, &input, (sp5_bytes){0}, now_us,
                               (sp5_optional_u64){.present = true, .value = 1234}, response)
              .status == SP5_OK);
    *request_id = sp5_pairing_read(state, now_us).request_id;
    return 0;
}

static int from_hex(sp5_bytes value, uint8_t *output, size_t capacity, size_t *size)
{
    CHECK(value.size % 2 == 0 && value.size / 2 <= capacity);
    for (size_t i = 0; i < value.size / 2; ++i) {
        unsigned high =
            value.data[2 * i] <= '9' ? value.data[2 * i] - '0' : value.data[2 * i] - 'a' + 10;
        unsigned low = value.data[2 * i + 1] <= '9' ? value.data[2 * i + 1] - '0'
                                                    : value.data[2 * i + 1] - 'a' + 10;
        CHECK(high < 16 && low < 16);
        output[i] = (uint8_t)((high << 4) | low);
    }
    *size = value.size / 2;
    return 0;
}

static sp5_bytes field(sp5_http_response *response, const char *tag)
{
    char opening[64], closing[64];
    int open_size = snprintf(opening, sizeof(opening), "<%s>", tag);
    int close_size = snprintf(closing, sizeof(closing), "</%s>", tag);
    if (open_size <= 0 || close_size <= 0 || (size_t)open_size >= sizeof(opening) ||
        (size_t)close_size >= sizeof(closing)) {
        return (sp5_bytes){0};
    }
    sp5_buffer fields = response->fields;
    for (size_t i = 0; i + (size_t)open_size <= fields.size; ++i) {
        if (memcmp(fields.data + i, opening, (size_t)open_size) != 0) {
            continue;
        }
        size_t start = i + (size_t)open_size;
        for (size_t j = start; j + (size_t)close_size <= fields.size; ++j) {
            if (memcmp(fields.data + j, closing, (size_t)close_size) == 0) {
                return (sp5_bytes){fields.data + start, j - start};
            }
        }
    }
    return (sp5_bytes){0};
}

static int aes(const uint8_t key[16], sp5_bytes input, uint8_t *output, bool encrypt)
{
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    CHECK(context != NULL);
    CHECK(EVP_CipherInit_ex(context, EVP_aes_128_ecb(), NULL, key, NULL, encrypt ? 1 : 0) == 1);
    CHECK(EVP_CIPHER_CTX_set_padding(context, 0) == 1);
    int written = 0, final = 0;
    CHECK(input.size <= 48);
    CHECK(EVP_CipherUpdate(context, output, &written, input.data, (int)input.size) == 1);
    CHECK(EVP_CipherFinal_ex(context, output + written, &final) == 1);
    CHECK((size_t)(written + final) == input.size);
    EVP_CIPHER_CTX_free(context);
    return 0;
}

static int challenge_digest(identity *who, sp5_bytes challenge, sp5_bytes secret, uint8_t out[32])
{
    const ASN1_BIT_STRING *signature = NULL;
    X509_get0_signature(&signature, NULL, who->certificate);
    CHECK(signature != NULL && signature->length > 0 && signature->length <= 512);
    uint8_t input[544];
    CHECK(challenge.size == 16 && secret.size == 16);
    memcpy(input, challenge.data, 16);
    memcpy(input + 16, signature->data, (size_t)signature->length);
    memcpy(input + 16 + (size_t)signature->length, secret.data, 16);
    size_t size = 0;
    CHECK(EVP_Q_digest(NULL, "SHA256", NULL, input, (size_t)signature->length + 32, out, &size) ==
          1);
    CHECK(size == 32);
    return 0;
}

/* Official Moonlight sends GET /unpair after any failure, without step 4. */
static int unpair(sp5_pair_flow *flow, sp5_http_response *response, const char *unique_id,
                  uint64_t now_us)
{
    char target[128];
    int size = snprintf(target, sizeof(target), "/unpair?uniqueid=%s&uuid=0011223344", unique_id);
    CHECK(size > 0 && (size_t)size < sizeof(target));
    sp5_request input = {.method = bytes("GET"), .target = {(const uint8_t *)target, (size_t)size}};
    response->fields.size = 0;
    response->code = 0;
    response->deferred = true;
    CHECK(
        sp5_pair_flow_handle(flow, &input, (sp5_bytes){0}, now_us, (sp5_optional_u64){0}, response)
            .status == SP5_OK);
    /* An empty root reveals no pairing state to this unauthenticated request. */
    CHECK(response->code == 200 && !response->deferred && response->fields.size == 0);
    return 0;
}

typedef enum {
    GOOD_PROOF,
    BAD_SIGNATURE,
    WRONG_PIN,
    OFFICIAL_WRONG_PIN,
    PERSIST_ERROR,
    NOTIFY_ERROR
} proof_case;

static int exchange(sp5_pair_flow *flow, sp5_pairing *state, identity *server, identity *client,
                    sp5_http_response *response, uint64_t now_us, files *storage,
                    proof_case outcome)
{
    uint64_t request_id = 0;
    CHECK(start(flow, state, client, response, now_us, &request_id) == 0);
    CHECK(response->code == 200 && response->deferred);
    CHECK(sp5_pair_flow_submit_pin(flow, request_id + 1, now_us + 1, bytes("1234"), bytes("Device"))
              .status == SP5_ERROR);
    CHECK(sp5_pair_flow_submit_pin(flow, request_id, now_us + 1, bytes("123x"), bytes("Device"))
              .status == SP5_ERROR);
    CHECK(sp5_pair_flow_submit_pin(flow, request_id, now_us + 1, bytes("1234"), bytes("Device <&>"))
              .status == SP5_OK);
    response->fields.size = 0;
    CHECK(sp5_pair_flow_take_certificate(flow, request_id, now_us + 2, response).status == SP5_OK);
    CHECK(!response->deferred && field(response, "plaincert").size == server->cert_size * 2);
    CHECK(sp5_pair_flow_take_certificate(flow, request_id, now_us + 2, response).status ==
          SP5_ERROR);
    uint8_t salt[16], key[16];
    for (size_t i = 0; i < sizeof(salt); ++i) {
        salt[i] = (uint8_t)i;
    }
    sp5_buffer key_buffer = {key, sizeof(key), 0};
    bool wrong_pin = outcome == WRONG_PIN || outcome == OFFICIAL_WRONG_PIN;
    CHECK(
        sp5_pairing_pin_key((sp5_bytes){salt, 16}, bytes(wrong_pin ? "9999" : "1234"), &key_buffer)
            .status == SP5_OK);
    const uint8_t client_challenge[16] = {1, 2, 3};
    const uint8_t client_secret[16] = {4, 5, 6};
    uint8_t encrypted[64], decrypted[64], server_secret[528], expected[32];
    CHECK(aes(key, (sp5_bytes){client_challenge, 16}, encrypted, true) == 0);
    CHECK(request(flow, response, "clientchallenge", (sp5_bytes){encrypted, 16}, now_us + 3) == 0);
    CHECK(response->code == 200);
    uint8_t reply[64];
    size_t reply_size = 0;
    CHECK(from_hex(field(response, "challengeresponse"), reply, sizeof(reply), &reply_size) == 0);
    CHECK(reply_size == 48);
    CHECK(aes(key, (sp5_bytes){reply, reply_size}, decrypted, false) == 0);
    CHECK(challenge_digest(client, (sp5_bytes){decrypted + 32, 16}, (sp5_bytes){client_secret, 16},
                           expected) == 0);
    CHECK(aes(key, (sp5_bytes){expected, 32}, encrypted, true) == 0);
    CHECK(request(flow, response, "serverchallengeresp", (sp5_bytes){encrypted, 32}, now_us + 4) ==
          0);
    CHECK(response->code == 200);
    size_t secret_size = 0;
    CHECK(from_hex(field(response, "pairingsecret"), server_secret, sizeof(server_secret),
                   &secret_size) == 0);
    CHECK(secret_size == 272);
    EVP_MD_CTX *verify = EVP_MD_CTX_new();
    CHECK(verify != NULL);
    CHECK(EVP_DigestVerifyInit(verify, NULL, EVP_sha256(), NULL, server->private_key) == 1);
    CHECK(EVP_DigestVerify(verify, server_secret + 16, secret_size - 16, server_secret, 16) == 1);
    EVP_MD_CTX_free(verify);
    CHECK(challenge_digest(server, (sp5_bytes){client_challenge, 16},
                           (sp5_bytes){server_secret, 16}, expected) == 0);
    CHECK((CRYPTO_memcmp(expected, decrypted, 32) == 0) == !wrong_pin);
    if (outcome == OFFICIAL_WRONG_PIN) {
        /* Client check B failed: moonlight-qt nvpairingmanager.cpp:339-348 unpairs
         * and never sends clientpairingsecret (protocol-notes.md 13.5). */
        CHECK(unpair(flow, response, "fixture", now_us + 5) == 0);
        CHECK(sp5_pairing_read(state, now_us + 5).state == SP5_PAIR_FAILED);
        CHECK(!sp5_pair_flow_pin_entered(flow, request_id));
        return 0;
    }
    uint8_t client_proof[528];
    memcpy(client_proof, client_secret, 16);
    EVP_MD_CTX *sign = EVP_MD_CTX_new();
    CHECK(sign != NULL);
    CHECK(EVP_DigestSignInit(sign, NULL, EVP_sha256(), NULL, client->private_key) == 1);
    size_t signature_size = sizeof(client_proof) - 16;
    CHECK(EVP_DigestSign(sign, client_proof + 16, &signature_size, client_secret, 16) == 1);
    EVP_MD_CTX_free(sign);
    if (outcome == BAD_SIGNATURE) {
        client_proof[16] ^= 1;
    }
    storage->fail_write = outcome == PERSIST_ERROR;
    storage->fail_notify = outcome == NOTIFY_ERROR;
    CHECK(request(flow, response, "clientpairingsecret",
                  (sp5_bytes){client_proof, signature_size + 16}, now_us + 5) == 0);
    storage->fail_write = false;
    storage->fail_notify = false;
    bool accepted = outcome == GOOD_PROOF || outcome == NOTIFY_ERROR;
    CHECK(response->code == (outcome == PERSIST_ERROR ? 500U : 200U));
    CHECK(field(response, "paired").size == 1);
    CHECK(field(response, "paired").data[0] == (uint8_t)(accepted ? '1' : '0'));
    CHECK(sp5_pairing_read(state, now_us + 5).state ==
          (accepted ? SP5_PAIR_COMPLETED : SP5_PAIR_FAILED));
    return 0;
}

/* T056 official-client wrong-PIN/abort path and host-fault accounting (FR-009/FR-010). */
static int official_abort(sp5_pair_flow *flow, sp5_pairing *state, sp5_devices *devices,
                          identity *server, identity *client, sp5_http_response *response,
                          files *storage)
{
    const uint64_t t = UINT64_C(500000000);
    sp5_bytes certificate = {client->cert, client->cert_size};
    CHECK(exchange(flow, state, server, client, response, t, storage, GOOD_PROOF) == 0);
    CHECK(sp5_pairing_read(state, t + 5).failures == 0);
    /* Unauthenticated /unpair never touches the paired-device registry. */
    CHECK(unpair(flow, response, "fixture", t + 10) == 0);
    CHECK(sp5_devices_authorized(devices, certificate));
    CHECK(sp5_pairing_read(state, t + 10).state == SP5_PAIR_COMPLETED);
    CHECK(sp5_devices_revoke(devices, certificate).status == SP5_OK);

    /* Before any PIN is entered, an abort ends the request without counting. */
    uint64_t id = 0;
    CHECK(start(flow, state, client, response, t + 20, &id) == 0 && response->deferred);
    CHECK(unpair(flow, response, "someone-else", t + 21) == 0);
    CHECK(sp5_pairing_read(state, t + 21).state == SP5_PAIR_PENDING);
    CHECK(unpair(flow, response, "fixture", t + 22) == 0);
    sp5_pair_view view = sp5_pairing_read(state, t + 22);
    CHECK(view.state == SP5_PAIR_FAILED && view.failures == 0 && view.request_id == id);
    CHECK(sp5_pair_flow_take_certificate(flow, id, t + 23, response).status == SP5_OK);
    CHECK(response->code == 400 && field(response, "paired").size == 1 &&
          field(response, "paired").data[0] == '0');
    CHECK(unpair(flow, response, "fixture", t + 24) == 0);
    CHECK(sp5_pairing_read(state, t + 24).failures == 0);

    /* Abort after the PIN was entered but before its certificate left the host. */
    CHECK(start(flow, state, client, response, t + 30, &id) == 0 && response->deferred);
    CHECK(sp5_pair_flow_submit_pin(flow, id, t + 31, bytes("1234"), bytes("Device")).status ==
          SP5_OK);
    CHECK(unpair(flow, response, "fixture", t + 32) == 0);
    view = sp5_pairing_read(state, t + 32);
    CHECK(view.state == SP5_PAIR_FAILED && view.failures == 0);

    /* A malformed or out-of-order step before the PIN key is in use ends the
     * request without counting toward the five-failure cooldown (FR-010). */
    CHECK(start(flow, state, client, response, t + 33, &id) == 0 && response->deferred);
    CHECK(request(flow, response, "clientchallenge", (sp5_bytes){client->cert, 16}, t + 34) == 0);
    view = sp5_pairing_read(state, t + 34);
    CHECK(response->code == 400 && view.state == SP5_PAIR_FAILED && view.failures == 0);
    CHECK(start(flow, state, client, response, t + 35, &id) == 0 && response->deferred);
    CHECK(sp5_pair_flow_submit_pin(flow, id, t + 36, bytes("1234"), bytes("Device")).status ==
          SP5_OK);
    CHECK(request(flow, response, "serverchallengeresp", (sp5_bytes){client->cert, 32}, t + 37) ==
          0);
    view = sp5_pairing_read(state, t + 37);
    CHECK(response->code == 400 && view.state == SP5_PAIR_FAILED && view.failures == 0);

    /* Host-side faults are not failed PIN entries. */
    storage->fail_notify = true;
    CHECK(start(flow, state, client, response, t + 40, &id) == 0 && response->code == 503);
    storage->fail_notify = false;
    view = sp5_pairing_read(state, t + 40);
    CHECK(view.state == SP5_PAIR_FAILED && view.failures == 0);
    CHECK(exchange(flow, state, server, client, response, t + 50, storage, PERSIST_ERROR) == 0);
    CHECK(sp5_pairing_read(state, t + 55).failures == 0);
    CHECK(!sp5_devices_authorized(devices, certificate));

    /* Official wrong PIN: counted once and immediately retryable, never a 409. */
    for (unsigned failure = 1; failure <= 5; ++failure) {
        uint64_t at = t + 100 + (uint64_t)failure * 10;
        CHECK(exchange(flow, state, server, client, response, at, storage, OFFICIAL_WRONG_PIN) ==
              0);
        view = sp5_pairing_read(state, at + 5);
        CHECK(view.state == SP5_PAIR_FAILED && view.failures == failure);
        CHECK((view.cooldown_until_us != 0) == (failure == 5));
    }
    CHECK(!sp5_devices_authorized(devices, certificate));
    uint64_t cooldown = t + 155 + UINT64_C(60000000);
    CHECK(sp5_pairing_read(state, t + 160).cooldown_until_us == cooldown);
    CHECK(start(flow, state, client, response, t + 160, &id) == 0 && response->code == 429);
    CHECK(start(flow, state, client, response, cooldown - 1, &id) == 0 && response->code == 429);
    CHECK(exchange(flow, state, server, client, response, cooldown, storage, GOOD_PROOF) == 0);
    CHECK(sp5_pairing_read(state, cooldown + 5).failures == 0);
    CHECK(sp5_devices_authorized(devices, certificate));
    CHECK(sp5_devices_revoke(devices, certificate).status == SP5_OK);
    return 0;
}

int main(void)
{
    identity server = {0}, client = {0};
    CHECK(make_identity(&server) == 0 && make_identity(&client) == 0);
    files storage = {0};
    sp5_storage_ops ops = {&storage, read_file, write_file, NULL};
    sp5_devices *devices = sp5_devices_create(&ops);
    CHECK(devices != NULL && sp5_devices_load(devices).status == SP5_OK);
    sp5_pairing *state = sp5_pairing_create();
    CHECK(state != NULL);
    sp5_pair_flow_config config = {.state = state,
                                   .devices = devices,
                                   .certificate_pem = {server.cert, server.cert_size},
                                   .private_key_pem = {server.key, server.key_size},
                                   .system = {.context = &storage, .notify = notify},
                                   .pin_url = "http://10.0.0.2:47990"};
    sp5_pair_flow *flow = sp5_pair_flow_create(&config);
    CHECK(flow != NULL);
    uint8_t fields[8192];
    sp5_http_response response = {.fields = {fields, sizeof(fields), 0}};
    CHECK(exchange(flow, state, &server, &client, &response, 1, &storage, BAD_SIGNATURE) == 0);
    CHECK(sp5_devices_count(devices) == 0 && storage.notifications == 1);
    CHECK(exchange(flow, state, &server, &client, &response, 10, &storage, GOOD_PROOF) == 0);
    sp5_bytes certificate = {client.cert, client.cert_size};
    CHECK(sp5_devices_authorized(devices, certificate));
    CHECK(storage.notifications == 3 && strstr(storage.last_notification, "Device <&>") != NULL);
    CHECK(sp5_pair_flow_diagnostic(flow) == SP5_REASON_NONE);
    sp5_devices *reloaded = sp5_devices_create(&ops);
    CHECK(reloaded != NULL && sp5_devices_load(reloaded).status == SP5_OK);
    CHECK(sp5_devices_authorized(reloaded, certificate));
    sp5_devices_destroy(reloaded);
    sp5_request confirm = {.method = bytes("GET"),
                           .target = bytes("/pair?uniqueid=fixture&phrase=pairchallenge")};
    CHECK(sp5_pair_flow_handle(flow, &confirm, (sp5_bytes){0}, 20, (sp5_optional_u64){0}, &response)
                  .status == SP5_OK &&
          response.code == 403);
    CHECK(sp5_pair_flow_handle(flow, &confirm, certificate, 20, (sp5_optional_u64){0}, &response)
                  .status == SP5_OK &&
          response.code == 200);
    CHECK(sp5_devices_revoke(devices, certificate).status == SP5_OK);
    uint64_t id = 0;
    CHECK(start(flow, state, &client, &response, 30, &id) == 0 && response.deferred);
    uint64_t previous = id;
    CHECK(start(flow, state, &client, &response, 31, &id) == 0 && response.code == 409);
    CHECK(id == previous);
    CHECK(sp5_pair_flow_submit_pin(flow, id, 29, bytes("1234"), bytes("Name")).reason ==
          SP5_REASON_CLOCK);
    CHECK(sp5_pair_flow_tick(flow, UINT64_C(300000030)).status == SP5_OK);
    CHECK(sp5_pairing_read(state, UINT64_C(300000030)).state == SP5_PAIR_EXPIRED);
    CHECK(sp5_pair_flow_submit_pin(flow, id, UINT64_C(300000030), bytes("1234"), bytes("Name"))
              .status == SP5_ERROR);
    CHECK(sp5_pair_flow_take_certificate(flow, id, UINT64_C(300000030), &response).status ==
          SP5_OK);
    sp5_bytes expired_paired = field(&response, "paired");
    CHECK(response.code == 408 && expired_paired.size == 1 && expired_paired.data != NULL &&
          expired_paired.data[0] == '0');
    CHECK(exchange(flow, state, &server, &client, &response, UINT64_C(300000040), &storage,
                   WRONG_PIN) == 0);
    CHECK(!sp5_devices_authorized(devices, certificate));
    CHECK(exchange(flow, state, &server, &client, &response, UINT64_C(300000050), &storage,
                   PERSIST_ERROR) == 0);
    CHECK(!sp5_devices_authorized(devices, certificate));
    CHECK(sp5_pair_flow_diagnostic(flow) == SP5_REASON_PLATFORM);
    CHECK(exchange(flow, state, &server, &client, &response, UINT64_C(300000060), &storage,
                   NOTIFY_ERROR) == 0);
    CHECK(sp5_devices_authorized(devices, certificate));
    CHECK(sp5_pair_flow_diagnostic(flow) == SP5_REASON_PLATFORM);
    CHECK(sp5_devices_revoke(devices, certificate).status == SP5_OK);
    for (uint64_t failure = 0; failure < 5; ++failure) {
        CHECK(exchange(flow, state, &server, &client, &response, UINT64_C(300000070) + failure * 10,
                       &storage, BAD_SIGNATURE) == 0);
    }
    CHECK(sp5_pairing_read(state, UINT64_C(300000115)).failures == 5);
    CHECK(start(flow, state, &client, &response, UINT64_C(360000114), &id) == 0 &&
          response.code == 429);
    CHECK(start(flow, state, &client, &response, UINT64_C(360000115), &id) == 0 &&
          response.deferred);
    unsigned before_early_step = sp5_pairing_read(state, UINT64_C(360000115)).failures;
    CHECK(request(flow, &response, "serverchallengeresp", (sp5_bytes){server.cert, 32},
                  UINT64_C(360000116)) == 0);
    /* An out-of-order step before any PIN key is in use is not a failed PIN entry. */
    CHECK(response.code == 400 &&
          sp5_pairing_read(state, UINT64_C(360000116)).state == SP5_PAIR_FAILED &&
          sp5_pairing_read(state, UINT64_C(360000116)).failures == before_early_step);
    CHECK(!sp5_devices_authorized(devices, certificate));
    CHECK(official_abort(flow, state, devices, &server, &client, &response, &storage) == 0);
    sp5_pair_flow_destroy(flow);
    sp5_pairing_destroy(state);
    sp5_devices_destroy(devices);
    X509_free(server.certificate);
    X509_free(client.certificate);
    EVP_PKEY_free(server.private_key);
    EVP_PKEY_free(client.private_key);
    OPENSSL_cleanse(server.key, sizeof(server.key));
    OPENSSL_cleanse(client.key, sizeof(client.key));
    return 0;
}
