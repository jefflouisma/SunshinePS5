#include "pairing_flow.h"

#include <openssl/pem.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Gen7 SHA256/AES-ECB(no padding)/RSA-SHA256 compositions and phase order:
 * protocol-notes.md §§13.2-13.6; mqt nvpairingmanager.cpp:215-381 @ 8f2e52e,
 * sun nvhttp.cpp:753-913 @ d0b4a88. All primitives use pinned OpenSSL. */
enum { SP5_RSA_MAX_BYTES = 512, SP5_PAIR_UNIQUE_ID_CAPACITY = 128 };
typedef enum {
    PAIR_IDLE,
    PAIR_WAIT_PIN,
    PAIR_CERT_READY,
    PAIR_CLIENT_CHALLENGE,
    PAIR_SERVER_RESPONSE,
    PAIR_CLIENT_PROOF
} pair_phase;

struct sp5_pair_flow {
    sp5_pair_flow_config config;
    EVP_PKEY *private_key;
    X509 *certificate;
    uint8_t certificate_pem[SP5_CERTIFICATE_CAPACITY];
    size_t certificate_size;
    char pin_url[192];
    uint64_t last_us;
    sp5_reason diagnostic;
    pair_phase phase;
    struct {
        uint64_t id;
        uint8_t unique_id[SP5_PAIR_UNIQUE_ID_CAPACITY];
        size_t unique_id_size;
        uint8_t client_cert[SP5_CERTIFICATE_CAPACITY];
        size_t client_cert_size;
        uint8_t salt[16], key[16], challenge[16], secret[16], response[32];
        uint8_t name[SP5_NAME_CAPACITY];
        size_t name_size;
    } pending;
};

static sp5_bytes text(const char *value)
{
    return (sp5_bytes){(const uint8_t *)value, strlen(value)};
}

static bool equal(sp5_bytes value, const char *literal)
{
    return value.size == strlen(literal) && memcmp(value.data, literal, value.size) == 0;
}

static bool rsa_key(EVP_PKEY *key)
{
    return key != NULL && EVP_PKEY_is_a(key, "RSA") && EVP_PKEY_get_bits(key) >= 2048 &&
           EVP_PKEY_get_bits(key) <= SP5_RSA_MAX_BYTES * 8;
}

static X509 *parse_certificate(sp5_bytes pem)
{
    if (pem.data == NULL || pem.size == 0 || pem.size > SP5_CERTIFICATE_CAPACITY) {
        return NULL;
    }
    BIO *bio = BIO_new_mem_buf(pem.data, (int)pem.size);
    X509 *certificate = bio == NULL ? NULL : PEM_read_bio_X509(bio, NULL, NULL, NULL);
    if (certificate != NULL) {
        char *remaining = NULL;
        long size = BIO_get_mem_data(bio, &remaining);
        bool valid = size >= 0;
        for (long i = 0; i < size; ++i) {
            valid = valid && (remaining[i] == ' ' || remaining[i] == '\r' || remaining[i] == '\n' ||
                              remaining[i] == '\t');
        }
        if (!valid || !rsa_key(X509_get0_pubkey(certificate))) {
            X509_free(certificate);
            certificate = NULL;
        }
    }
    BIO_free(bio);
    return certificate;
}

static sp5_result canonical(X509 *certificate, sp5_buffer *output)
{
    BIO *bio = BIO_new(BIO_s_mem());
    sp5_result result = sp5_failure(SP5_REASON_PLATFORM);
    if (bio != NULL && PEM_write_bio_X509(bio, certificate) == 1) {
        char *data = NULL;
        long size = BIO_get_mem_data(bio, &data);
        if (size > 0) {
            result = sp5_buffer_append(output, (sp5_bytes){(const uint8_t *)data, (size_t)size});
        }
    }
    BIO_free(bio);
    return result;
}

sp5_result sp5_pairing_pin_key(sp5_bytes salt, sp5_bytes pin, sp5_buffer *key)
{
    if (salt.data == NULL || pin.data == NULL || salt.size != 16 || pin.size != 4 || key == NULL ||
        key->data == NULL || key->size > key->capacity) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (key->capacity < 16) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    uint8_t input[20], digest[32];
    for (size_t i = 0; i < pin.size; ++i) {
        if (pin.data[i] < '0' || pin.data[i] > '9') {
            return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
        }
    }
    memcpy(input, salt.data, 16);
    memcpy(input + 16, pin.data, 4);
    size_t size = 0;
    sp5_result result = sp5_failure(SP5_REASON_PLATFORM);
    if (EVP_Q_digest(NULL, "SHA256", NULL, input, sizeof(input), digest, &size) == 1 &&
        size == sizeof(digest)) {
        memmove(key->data, digest, 16);
        key->size = 16;
        result = sp5_success();
    }
    OPENSSL_cleanse(input, sizeof(input));
    OPENSSL_cleanse(digest, sizeof(digest));
    return result;
}

static sp5_result digest_challenge(X509 *certificate, sp5_bytes challenge, sp5_bytes secret,
                                   uint8_t digest[32])
{
    const ASN1_BIT_STRING *signature = NULL;
    X509_get0_signature(&signature, NULL, certificate);
    if (signature == NULL || signature->length <= 0 || signature->length > SP5_RSA_MAX_BYTES ||
        challenge.size != 16 || secret.size != 16 || challenge.data == NULL ||
        secret.data == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    uint8_t input[SP5_RSA_MAX_BYTES + 32];
    memcpy(input, challenge.data, 16);
    memcpy(input + 16, signature->data, (size_t)signature->length);
    memcpy(input + 16 + (size_t)signature->length, secret.data, 16);
    size_t size = 0;
    bool valid = EVP_Q_digest(NULL, "SHA256", NULL, input, (size_t)signature->length + 32, digest,
                              &size) == 1 &&
                 size == 32;
    OPENSSL_cleanse(input, sizeof(input));
    return valid ? sp5_success() : sp5_failure(SP5_REASON_PLATFORM);
}

sp5_result sp5_pairing_verify_client(sp5_bytes certificate_pem, sp5_bytes server_challenge,
                                     sp5_bytes decrypted_response, sp5_bytes signed_client_secret)
{
    const struct {
        sp5_bytes certificate, challenge, response, proof;
    } exchange = {certificate_pem, server_challenge, decrypted_response, signed_client_secret};
    if (exchange.challenge.data == NULL || exchange.challenge.size != 16 ||
        exchange.response.data == NULL || exchange.response.size != 32 ||
        exchange.proof.data == NULL || exchange.proof.size <= 16 ||
        exchange.proof.size > 16 + SP5_RSA_MAX_BYTES) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    X509 *certificate = parse_certificate(exchange.certificate);
    EVP_MD_CTX *verify = EVP_MD_CTX_new();
    uint8_t expected[32];
    bool valid = false;
    if (certificate != NULL && verify != NULL) {
        EVP_PKEY *key = X509_get0_pubkey(certificate);
        valid = exchange.proof.size == (size_t)EVP_PKEY_get_size(key) + 16 &&
                digest_challenge(certificate, exchange.challenge,
                                 (sp5_bytes){exchange.proof.data, 16}, expected)
                        .status == SP5_OK &&
                CRYPTO_memcmp(expected, exchange.response.data, sizeof(expected)) == 0 &&
                EVP_DigestVerifyInit(verify, NULL, EVP_sha256(), NULL, key) == 1 &&
                EVP_DigestVerify(verify, exchange.proof.data + 16, exchange.proof.size - 16,
                                 exchange.proof.data, 16) == 1;
    }
    OPENSSL_cleanse(expected, sizeof(expected));
    EVP_MD_CTX_free(verify);
    X509_free(certificate);
    return valid ? sp5_success() : sp5_failure(SP5_REASON_OWNERSHIP);
}

static bool valid_pin_url(const char *pin_url)
{
    if (pin_url == NULL || strlen(pin_url) >= sizeof(((sp5_pair_flow *)NULL)->pin_url)) {
        return false;
    }
    for (size_t i = 0; pin_url[i] != 0; ++i) {
        if ((unsigned char)pin_url[i] <= 32 || (unsigned char)pin_url[i] >= 127) {
            return false;
        }
    }
    return true;
}

sp5_pair_flow *sp5_pair_flow_create(const sp5_pair_flow_config *config)
{
    if (config == NULL || config->state == NULL || config->devices == NULL ||
        config->system.notify == NULL || config->private_key_pem.data == NULL ||
        config->private_key_pem.size == 0 || config->private_key_pem.size > SP5_IDENTITY_CAPACITY ||
        !valid_pin_url(config->pin_url)) {
        return NULL;
    }
    sp5_pair_flow *flow = calloc(1, sizeof(*flow));
    if (flow == NULL) {
        return NULL;
    }
    flow->config = *config;
    flow->certificate = parse_certificate(config->certificate_pem);
    BIO *key = BIO_new_mem_buf(config->private_key_pem.data, (int)config->private_key_pem.size);
    flow->private_key = key == NULL ? NULL : PEM_read_bio_PrivateKey(key, NULL, NULL, NULL);
    BIO_free(key);
    sp5_buffer pem = {flow->certificate_pem, sizeof(flow->certificate_pem), 0};
    if (flow->certificate == NULL || !rsa_key(flow->private_key) ||
        X509_check_private_key(flow->certificate, flow->private_key) != 1 ||
        canonical(flow->certificate, &pem).status != SP5_OK) {
        sp5_pair_flow_destroy(flow);
        return NULL;
    }
    flow->certificate_size = pem.size;
    memcpy(flow->pin_url, config->pin_url, strlen(config->pin_url) + 1);
    /* Borrow only live owner interfaces; copied identity/URL avoid retaining
     * caller scratch PEM buffers (especially private-key material). */
    flow->config.certificate_pem = (sp5_bytes){0};
    flow->config.private_key_pem = (sp5_bytes){0};
    flow->config.pin_url = NULL;
    return flow;
}

bool sp5_pair_flow_set_pin_url(sp5_pair_flow *flow, const char *pin_url)
{
    if (flow == NULL || !valid_pin_url(pin_url)) {
        return false;
    }
    memmove(flow->pin_url, pin_url, strlen(pin_url) + 1);
    return true;
}

void sp5_pair_flow_destroy(sp5_pair_flow *flow)
{
    if (flow != NULL) {
        EVP_PKEY_free(flow->private_key);
        X509_free(flow->certificate);
        OPENSSL_cleanse(flow, sizeof(*flow));
        free(flow);
    }
}

static void erase_pending(sp5_pair_flow *flow)
{
    OPENSSL_cleanse(&flow->pending, sizeof(flow->pending));
    flow->phase = PAIR_IDLE;
}

sp5_result sp5_pair_flow_tick(sp5_pair_flow *flow, uint64_t now_us)
{
    if (flow == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    if (now_us < flow->last_us) {
        return sp5_failure(SP5_REASON_CLOCK);
    }
    flow->last_us = now_us;
    sp5_pair_view state = sp5_pairing_read(flow->config.state, now_us);
    if (state.state != SP5_PAIR_PENDING || state.request_id != flow->pending.id) {
        erase_pending(flow);
    }
    return sp5_success();
}

bool sp5_pair_flow_pin_entered(const sp5_pair_flow *flow, uint64_t request_id)
{
    return flow != NULL && request_id != 0 && flow->pending.id == request_id &&
           flow->phase > PAIR_WAIT_PIN;
}

sp5_reason sp5_pair_flow_diagnostic(const sp5_pair_flow *flow)
{
    return flow == NULL ? SP5_REASON_INVALID_ARGUMENT : flow->diagnostic;
}

static sp5_result paired(sp5_http_response *response, unsigned code, bool success)
{
    response->code = code;
    response->deferred = false;
    response->fields.size = 0;
    return sp5_http_field(response, "paired", text(success ? "1" : "0"));
}

/* FR-010 counts failed PIN entries only: a rejected or aborted proof from the
 * client once the PIN-derived key is in use. Notification, crypto, clock,
 * capacity and persistence faults on the host, and malformed or out-of-order
 * steps before the PIN key is in use, end the request without counting toward
 * the five-failure cooldown. */
typedef enum { FAILED_PROOF, HOST_FAULT, EARLY_STEP } failure_kind;
typedef struct {
    uint64_t now_us;
    unsigned code;
    failure_kind kind;
} pair_failure;

static sp5_result fail_pair(sp5_pair_flow *flow, pair_failure failure, sp5_http_response *response)
{
    sp5_result result =
        failure.kind == FAILED_PROOF
            ? sp5_pairing_finish(flow->config.state, flow->pending.id, failure.now_us, false)
            : sp5_pairing_cancel(flow->config.state, flow->pending.id, failure.now_us);
    if (result.status != SP5_OK && result.reason != SP5_REASON_OWNERSHIP &&
        result.reason != SP5_REASON_TIMEOUT) {
        flow->diagnostic = result.reason;
    }
    erase_pending(flow);
    return paired(response, result.reason == SP5_REASON_TIMEOUT ? 408 : failure.code, false);
}

/* A rejected step counts as a failed PIN entry only once the PIN key is in use. */
static failure_kind step_failure(const sp5_pair_flow *flow)
{
    return flow->phase >= PAIR_CLIENT_CHALLENGE ? FAILED_PROOF : EARLY_STEP;
}

static sp5_result query(const sp5_request *request, const char *key, sp5_buffer *value,
                        bool required)
{
    bool present = false;
    sp5_result result = sp5_http_query(request, key, value, &present);
    if (result.status != SP5_OK) {
        return result;
    }
    return required && (!present || value->size == 0) ? sp5_failure(SP5_REASON_INVALID_ARGUMENT)
                                                      : sp5_success();
}

static sp5_result unhex(sp5_bytes input, sp5_buffer *output)
{
    if (input.size % 2 != 0 || input.size / 2 > output->capacity) {
        return sp5_failure(SP5_REASON_CAPACITY);
    }
    output->size = 0;
    for (size_t i = 0; i < input.size; i += 2) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; ++j) {
            uint8_t digit = input.data[i + j];
            unsigned half;
            if (digit >= '0' && digit <= '9') {
                half = digit - '0';
            } else if (digit >= 'a' && digit <= 'f') {
                half = digit - 'a' + 10;
            } else if (digit >= 'A' && digit <= 'F') {
                half = digit - 'A' + 10;
            } else {
                return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
            }
            value = (value << 4) | half;
        }
        output->data[output->size++] = (uint8_t)value;
    }
    return sp5_success();
}

static sp5_result aes(const uint8_t key[16], sp5_bytes input, uint8_t *output, bool encrypt)
{
    if (input.data == NULL || input.size == 0 || input.size > 48 || input.size % 16 != 0) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    int size = 0, final = 0;
    bool valid =
        context != NULL &&
        EVP_CipherInit_ex(context, EVP_aes_128_ecb(), NULL, key, NULL, encrypt ? 1 : 0) == 1 &&
        EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
        EVP_CipherUpdate(context, output, &size, input.data, (int)input.size) == 1 &&
        EVP_CipherFinal_ex(context, output + size, &final) == 1 &&
        (size_t)size + (size_t) final == input.size;
    EVP_CIPHER_CTX_free(context);
    return valid ? sp5_success() : sp5_failure(SP5_REASON_PLATFORM);
}

static sp5_result begin_pair(sp5_pair_flow *flow, const sp5_request *request, sp5_bytes unique_id,
                             uint64_t now_us, sp5_optional_u64 wall_us, sp5_http_response *response)
{
    sp5_pair_view state = sp5_pairing_read(flow->config.state, now_us);
    if (state.state == SP5_PAIR_PENDING) {
        return paired(response, 409, false);
    }
    if (now_us < state.cooldown_until_us) {
        return paired(response, 429, false);
    }
    uint8_t encoded[SP5_CERTIFICATE_CAPACITY * 2], decoded[SP5_CERTIFICATE_CAPACITY];
    sp5_buffer value = {encoded, sizeof(encoded), 0};
    sp5_buffer pem = {decoded, sizeof(decoded), 0};
    if (query(request, "clientcert", &value, true).status != SP5_OK ||
        unhex((sp5_bytes){value.data, value.size}, &pem).status != SP5_OK) {
        return paired(response, 400, false);
    }
    X509 *certificate = parse_certificate((sp5_bytes){pem.data, pem.size});
    if (certificate == NULL) {
        return paired(response, 400, false);
    }
    sp5_buffer client = {flow->pending.client_cert, sizeof(flow->pending.client_cert), 0};
    sp5_result result = canonical(certificate, &client);
    X509_free(certificate);
    if (result.status != SP5_OK) {
        erase_pending(flow);
        return result;
    }
    flow->pending.client_cert_size = client.size;
    sp5_buffer salt = {flow->pending.salt, sizeof(flow->pending.salt), 0};
    if (query(request, "salt", &value, true).status != SP5_OK || value.size != 32 ||
        unhex((sp5_bytes){value.data, value.size}, &salt).status != SP5_OK) {
        erase_pending(flow);
        return paired(response, 400, false);
    }
    value.size = 0;
    if (query(request, "devicename", &value, false).status != SP5_OK ||
        value.size > SP5_PAIR_CLIENT_NAME_CAPACITY) {
        erase_pending(flow);
        return paired(response, 400, false);
    }
    result = sp5_pairing_begin_request(flow->config.state, now_us, wall_us,
                                       (sp5_bytes){value.data, value.size}, &flow->pending.id);
    if (result.status != SP5_OK) {
        erase_pending(flow);
        return paired(response, result.reason == SP5_REASON_TIMEOUT ? 429 : 409, false);
    }
    flow->diagnostic = SP5_REASON_NONE;
    memcpy(flow->pending.unique_id, unique_id.data, unique_id.size);
    flow->pending.unique_id_size = unique_id.size;
    flow->phase = PAIR_WAIT_PIN;
    char message[320];
    int size = snprintf(message, sizeof(message),
                        "A device wants to pair (request %llu). Enter its PIN at %s.",
                        (unsigned long long)flow->pending.id, flow->pin_url);
    if (size < 0 || (size_t)size >= sizeof(message)) {
        flow->diagnostic = SP5_REASON_CAPACITY;
        return fail_pair(flow, (pair_failure){now_us, 503, HOST_FAULT}, response);
    }
    result = flow->config.system.notify(flow->config.system.context, message, (size_t)size);
    if (result.status != SP5_OK) {
        flow->diagnostic = result.reason;
        return fail_pair(flow, (pair_failure){now_us, 503, HOST_FAULT}, response);
    }
    response->fields.size = 0;
    response->code = 200;
    response->deferred = true;
    return sp5_success();
}

sp5_result sp5_pair_flow_submit_pin(sp5_pair_flow *flow, uint64_t request_id, uint64_t now_us,
                                    sp5_bytes pin, sp5_bytes device_name)
{
    struct {
        uint64_t request_id, now_us;
        sp5_bytes pin, device_name;
    } submission = {request_id, now_us, pin, device_name};
    sp5_result result = sp5_pair_flow_tick(flow, submission.now_us);
    if (result.status != SP5_OK) {
        return result;
    }
    if (flow->phase != PAIR_WAIT_PIN || flow->pending.id != submission.request_id) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    if (submission.device_name.size == 0) {
        submission.device_name = text("Moonlight device");
    }
    if (!sp5_name_valid(submission.device_name)) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    sp5_buffer key = {flow->pending.key, sizeof(flow->pending.key), 0};
    result = sp5_pairing_pin_key((sp5_bytes){flow->pending.salt, 16}, submission.pin, &key);
    if (result.status != SP5_OK) {
        return result;
    }
    memcpy(flow->pending.name, submission.device_name.data, submission.device_name.size);
    flow->pending.name_size = submission.device_name.size;
    flow->phase = PAIR_CERT_READY;
    return sp5_success();
}

sp5_result sp5_pair_flow_take_certificate(sp5_pair_flow *flow, uint64_t request_id, uint64_t now_us,
                                          sp5_http_response *response)
{
    const struct {
        uint64_t request_id, now_us;
    } submission = {request_id, now_us};
    if (response == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    sp5_result result = sp5_pair_flow_tick(flow, submission.now_us);
    if (result.status != SP5_OK) {
        return result;
    }
    sp5_pair_view state = sp5_pairing_read(flow->config.state, submission.now_us);
    if (submission.request_id == 0 || state.request_id != submission.request_id) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    if (state.state == SP5_PAIR_FAILED || state.state == SP5_PAIR_EXPIRED) {
        return paired(response, state.state == SP5_PAIR_EXPIRED ? 408 : 400, false);
    }
    if (flow->phase == PAIR_WAIT_PIN) {
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    }
    if (flow->phase != PAIR_CERT_READY) {
        return sp5_failure(SP5_REASON_OWNERSHIP);
    }
    result = paired(response, 200, true);
    if (result.status == SP5_OK) {
        result = sp5_http_field_hex(response, "plaincert",
                                    (sp5_bytes){flow->certificate_pem, flow->certificate_size});
    }
    if (result.status == SP5_OK) {
        flow->phase = PAIR_CLIENT_CHALLENGE;
    }
    return result;
}

static sp5_result client_challenge(sp5_pair_flow *flow, sp5_bytes input, uint64_t now_us,
                                   sp5_http_response *response)
{
    if (flow->phase != PAIR_CLIENT_CHALLENGE || input.size != 16) {
        return fail_pair(flow, (pair_failure){now_us, 400, step_failure(flow)}, response);
    }
    uint8_t challenge[16], plain[48], encrypted[64];
    sp5_result result = aes(flow->pending.key, input, challenge, false);
    if (result.status == SP5_OK && (RAND_bytes(flow->pending.challenge, 16) != 1 ||
                                    RAND_bytes(flow->pending.secret, 16) != 1)) {
        result = sp5_failure(SP5_REASON_PLATFORM);
    }
    if (result.status == SP5_OK) {
        result = digest_challenge(flow->certificate, (sp5_bytes){challenge, 16},
                                  (sp5_bytes){flow->pending.secret, 16}, plain);
    }
    memcpy(plain + 32, flow->pending.challenge, 16);
    if (result.status == SP5_OK) {
        result = aes(flow->pending.key, (sp5_bytes){plain, sizeof(plain)}, encrypted, true);
    }
    if (result.status == SP5_OK) {
        result = paired(response, 200, true);
    }
    if (result.status == SP5_OK) {
        result = sp5_http_field_hex(response, "challengeresponse", (sp5_bytes){encrypted, 48});
    }
    OPENSSL_cleanse(challenge, sizeof(challenge));
    OPENSSL_cleanse(plain, sizeof(plain));
    OPENSSL_cleanse(encrypted, sizeof(encrypted));
    if (result.status != SP5_OK) {
        flow->diagnostic = result.reason;
        return fail_pair(flow, (pair_failure){now_us, 503, HOST_FAULT}, response);
    }
    flow->phase = PAIR_SERVER_RESPONSE;
    return sp5_success();
}

static sp5_result server_response(sp5_pair_flow *flow, sp5_bytes input, uint64_t now_us,
                                  sp5_http_response *response)
{
    if (flow->phase != PAIR_SERVER_RESPONSE || input.size != 32) {
        return fail_pair(flow, (pair_failure){now_us, 400, step_failure(flow)}, response);
    }
    sp5_result result = aes(flow->pending.key, input, flow->pending.response, false);
    uint8_t proof[16 + SP5_RSA_MAX_BYTES];
    memcpy(proof, flow->pending.secret, 16);
    size_t signature_size = SP5_RSA_MAX_BYTES;
    EVP_MD_CTX *sign = EVP_MD_CTX_new();
    if (result.status == SP5_OK &&
        (sign == NULL ||
         EVP_DigestSignInit(sign, NULL, EVP_sha256(), NULL, flow->private_key) != 1 ||
         EVP_DigestSign(sign, proof + 16, &signature_size, proof, 16) != 1)) {
        result = sp5_failure(SP5_REASON_PLATFORM);
    }
    EVP_MD_CTX_free(sign);
    if (result.status == SP5_OK) {
        result = paired(response, 200, true);
    }
    if (result.status == SP5_OK) {
        result =
            sp5_http_field_hex(response, "pairingsecret", (sp5_bytes){proof, signature_size + 16});
    }
    OPENSSL_cleanse(proof, sizeof(proof));
    if (result.status != SP5_OK) {
        flow->diagnostic = result.reason;
        return fail_pair(flow, (pair_failure){now_us, 503, HOST_FAULT}, response);
    }
    flow->phase = PAIR_CLIENT_PROOF;
    return sp5_success();
}

static sp5_result client_proof(sp5_pair_flow *flow, sp5_bytes proof, uint64_t now_us,
                               sp5_optional_u64 wall_us, sp5_http_response *response)
{
    if (flow->phase != PAIR_CLIENT_PROOF) {
        return fail_pair(flow, (pair_failure){now_us, 400, FAILED_PROOF}, response);
    }
    sp5_bytes certificate = {flow->pending.client_cert, flow->pending.client_cert_size};
    sp5_result result =
        sp5_pairing_verify_client(certificate, (sp5_bytes){flow->pending.challenge, 16},
                                  (sp5_bytes){flow->pending.response, 32}, proof);
    if (result.status != SP5_OK) {
        return fail_pair(flow, (pair_failure){now_us, 200, FAILED_PROOF}, response);
    }
    if (!wall_us.present) {
        flow->diagnostic = SP5_REASON_CLOCK;
        return fail_pair(flow, (pair_failure){now_us, 503, HOST_FAULT}, response);
    }
    /* Prepare the response before persistence: buffer exhaustion cannot create
     * authorization that we then misleadingly report as a rejected proof. */
    result = paired(response, 200, true);
    if (result.status != SP5_OK) {
        return result;
    }
    result =
        sp5_devices_pair(flow->config.devices, certificate,
                         (sp5_bytes){flow->pending.name, flow->pending.name_size}, wall_us.value);
    if (result.status != SP5_OK) {
        flow->diagnostic = result.reason;
        return fail_pair(flow, (pair_failure){now_us, 500, HOST_FAULT}, response);
    }
    result = sp5_pairing_finish(flow->config.state, flow->pending.id, now_us, true);
    if (result.status != SP5_OK) {
        flow->diagnostic = result.reason;
        erase_pending(flow);
        return result;
    }
    char message[128];
    int size = snprintf(message, sizeof(message), "Paired device: %.*s",
                        (int)flow->pending.name_size, (const char *)flow->pending.name);
    if (size < 0 || (size_t)size >= sizeof(message)) {
        flow->diagnostic = SP5_REASON_CAPACITY;
    } else {
        result = flow->config.system.notify(flow->config.system.context, message, (size_t)size);
        if (result.status != SP5_OK) {
            flow->diagnostic = result.reason;
        }
    }
    erase_pending(flow);
    return sp5_success();
}

static bool unpair_target(sp5_bytes target)
{
    size_t size = 0;
    while (size < target.size && target.data[size] != '?') {
        ++size;
    }
    return equal((sp5_bytes){target.data, size}, "/unpair");
}

/* Official Moonlight checks the PIN itself after step 3 (client check B) and,
 * on a mismatch or any other failure, sends GET /unpair over HTTP instead of
 * step 4 (protocol-notes.md:1036-1037; mqt nvpairingmanager.cpp:339-348 @
 * 8f2e52e). Without this the request would stay pending, uncounted, for five
 * minutes. The request is unauthenticated: it only ends the matching pending
 * handshake, never touches the paired-device registry, and always answers an
 * empty 200 root so it reveals no pairing state. Once the host has released its
 * certificate for an entered PIN, the abort counts as a failed PIN (FR-010). */
static sp5_result client_abort(sp5_pair_flow *flow, const sp5_request *request, uint64_t now_us,
                               sp5_http_response *response)
{
    uint8_t unique_data[SP5_PAIR_UNIQUE_ID_CAPACITY];
    sp5_buffer unique_id = {unique_data, sizeof(unique_data), 0};
    bool matches = query(request, "uniqueid", &unique_id, true).status == SP5_OK &&
                   flow->phase != PAIR_IDLE && unique_id.size == flow->pending.unique_id_size &&
                   CRYPTO_memcmp(unique_id.data, flow->pending.unique_id, unique_id.size) == 0;
    if (matches) {
        sp5_result result =
            flow->phase >= PAIR_CLIENT_CHALLENGE
                ? sp5_pairing_finish(flow->config.state, flow->pending.id, now_us, false)
                : sp5_pairing_cancel(flow->config.state, flow->pending.id, now_us);
        if (result.status != SP5_OK && result.reason != SP5_REASON_OWNERSHIP &&
            result.reason != SP5_REASON_TIMEOUT) {
            flow->diagnostic = result.reason;
        }
        erase_pending(flow);
    }
    response->fields.size = 0;
    response->code = 200;
    response->deferred = false;
    return sp5_success();
}

sp5_result sp5_pair_flow_handle(void *context, const sp5_request *request,
                                sp5_bytes verified_certificate, uint64_t now_us,
                                sp5_optional_u64 wall_us, sp5_http_response *response)
{
    sp5_pair_flow *flow = context;
    if (request == NULL || response == NULL) {
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    }
    sp5_result result = sp5_pair_flow_tick(flow, now_us);
    if (result.status != SP5_OK) {
        return result;
    }
    response->fields.size = 0;
    response->code = 200;
    response->deferred = false;
    if (request->target.data != NULL && unpair_target(request->target)) {
        return client_abort(flow, request, now_us, response);
    }
    uint8_t unique_data[SP5_PAIR_UNIQUE_ID_CAPACITY], phrase_data[64];
    sp5_buffer unique_id = {unique_data, sizeof(unique_data), 0};
    sp5_buffer phrase = {phrase_data, sizeof(phrase_data), 0};
    if (query(request, "uniqueid", &unique_id, true).status != SP5_OK ||
        query(request, "phrase", &phrase, false).status != SP5_OK) {
        return paired(response, 400, false);
    }
    sp5_bytes phrase_value = {phrase.data, phrase.size};
    if (equal(phrase_value, "pairchallenge")) {
        bool authorized = verified_certificate.size != 0 &&
                          sp5_devices_authorized(flow->config.devices, verified_certificate);
        return paired(response, authorized ? 200 : 403, authorized);
    }
    if (equal(phrase_value, "getservercert")) {
        return begin_pair(flow, request, (sp5_bytes){unique_id.data, unique_id.size}, now_us,
                          wall_us, response);
    }
    if (flow->phase == PAIR_IDLE || unique_id.size != flow->pending.unique_id_size ||
        CRYPTO_memcmp(unique_id.data, flow->pending.unique_id, unique_id.size) != 0) {
        return paired(response, 400, false);
    }
    static const char *const keys[] = {"clientchallenge", "serverchallengeresp",
                                       "clientpairingsecret"};
    uint8_t encoded[(16 + SP5_RSA_MAX_BYTES) * 2], decoded[16 + SP5_RSA_MAX_BYTES];
    sp5_buffer value = {encoded, sizeof(encoded), 0};
    unsigned selected = 0, count = 0;
    for (unsigned i = 0; i < 3; ++i) {
        bool present = false;
        result = sp5_http_query(request, keys[i], &value, &present);
        if (result.status != SP5_OK) {
            return fail_pair(flow, (pair_failure){now_us, 400, step_failure(flow)}, response);
        }
        if (present) {
            selected = i;
            ++count;
        }
    }
    if (count != 1 || query(request, keys[selected], &value, true).status != SP5_OK) {
        return fail_pair(flow, (pair_failure){now_us, 400, step_failure(flow)}, response);
    }
    sp5_buffer input = {decoded, sizeof(decoded), 0};
    if (unhex((sp5_bytes){value.data, value.size}, &input).status != SP5_OK) {
        return fail_pair(flow, (pair_failure){now_us, 400, step_failure(flow)}, response);
    }
    sp5_bytes payload = {input.data, input.size};
    if (selected == 0) {
        result = client_challenge(flow, payload, now_us, response);
    } else if (selected == 1) {
        result = server_response(flow, payload, now_us, response);
    } else {
        result = client_proof(flow, payload, now_us, wall_us, response);
    }
    OPENSSL_cleanse(encoded, sizeof(encoded));
    OPENSSL_cleanse(decoded, sizeof(decoded));
    return result;
}
