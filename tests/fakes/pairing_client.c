/* SYNTHETIC client side only; see pairing_client.h. No official-client claim. */
#include "pairing_client.h"
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>

enum { SECRET_SIZE = 16, RSA_LIMIT = 512, TARGET_LIMIT = 8192 };
static const char unique_id[] = "synthetic-pair-client";
static const char hex_digits[] = "0123456789abcdef";

static sp5_result pem_bytes(BIO *bio, sp5_buffer *output)
{
    char *data = NULL;
    long size = BIO_get_mem_data(bio, &data);
    if (size <= 0)
        return sp5_failure(SP5_REASON_PLATFORM);
    return sp5_buffer_append(output, (sp5_bytes){(const uint8_t *)data, (size_t)size});
}

sp5_result sp5_fake_pair_identity(sp5_buffer *identity)
{
    if (identity == NULL || identity->data == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", (size_t)2048);
    X509 *certificate = X509_new();
    BIO *bio = BIO_new(BIO_s_mem());
    X509_NAME *name = certificate == NULL ? NULL : X509_get_subject_name(certificate);
    bool valid = key != NULL && certificate != NULL && bio != NULL && name != NULL &&
                 X509_set_version(certificate, 2) == 1 &&
                 ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) == 1 &&
                 X509_gmtime_adj(X509_getm_notBefore(certificate), -60) != NULL &&
                 X509_gmtime_adj(X509_getm_notAfter(certificate), 3600) != NULL &&
                 X509_set_pubkey(certificate, key) == 1 &&
                 X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                            (const unsigned char *)"Synthetic pairing fixture", -1,
                                            -1, 0) == 1 &&
                 X509_set_issuer_name(certificate, name) == 1 &&
                 X509_sign(certificate, key, EVP_sha256()) > 0 &&
                 PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL) == 1 &&
                 PEM_write_bio_X509(bio, certificate) == 1;
    sp5_result result = valid ? pem_bytes(bio, identity) : sp5_failure(SP5_REASON_PLATFORM);
    BIO_free(bio);
    X509_free(certificate);
    EVP_PKEY_free(key);
    return result;
}

void sp5_fake_pair_client_close(sp5_fake_pair_client *client)
{
    if (client == NULL)
        return;
    EVP_PKEY_free(client->key);
    X509_free(client->certificate);
    OPENSSL_cleanse(client, sizeof(*client));
}

sp5_result sp5_fake_pair_client_open(sp5_fake_pair_client *client, sp5_bytes identity)
{
    if (client == NULL || identity.data == NULL || identity.size == 0 || identity.size > INT32_MAX)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    memset(client, 0, sizeof(*client));
    BIO *key = BIO_new_mem_buf(identity.data, (int)identity.size);
    BIO *certificate = BIO_new_mem_buf(identity.data, (int)identity.size);
    BIO *canonical = BIO_new(BIO_s_mem());
    client->key = key == NULL ? NULL : PEM_read_bio_PrivateKey(key, NULL, NULL, NULL);
    client->certificate =
        certificate == NULL ? NULL : PEM_read_bio_X509(certificate, NULL, NULL, NULL);
    sp5_buffer pem = {client->certificate_pem, sizeof(client->certificate_pem), 0};
    sp5_result result = client->key != NULL && client->certificate != NULL && canonical != NULL &&
                                PEM_write_bio_X509(canonical, client->certificate) == 1
                            ? pem_bytes(canonical, &pem)
                            : sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    client->certificate_size = pem.size;
    BIO_free(key);
    BIO_free(certificate);
    BIO_free(canonical);
    if (result.status != SP5_OK)
        sp5_fake_pair_client_close(client);
    return result;
}

static bool append_hex(char *target, size_t capacity, size_t *size, sp5_bytes value)
{
    if (value.size > (capacity - *size - 1) / 2)
        return false;
    for (size_t i = 0; i < value.size; ++i) {
        target[(*size)++] = hex_digits[value.data[i] >> 4];
        target[(*size)++] = hex_digits[value.data[i] & 15];
    }
    target[*size] = 0;
    return true;
}

static bool append_text(char *target, size_t capacity, size_t *size, const char *text)
{
    size_t length = strlen(text);
    if (length >= capacity - *size)
        return false;
    memcpy(target + *size, text, length + 1);
    *size += length;
    return true;
}

static bool append_percent(char *target, size_t capacity, size_t *size, sp5_bytes value)
{
    for (size_t i = 0; i < value.size; ++i) {
        if (capacity - *size < 4)
            return false;
        target[(*size)++] = '%';
        target[(*size)++] = hex_digits[value.data[i] >> 4];
        target[(*size)++] = hex_digits[value.data[i] & 15];
    }
    target[*size] = 0;
    return true;
}

static sp5_bytes field(const sp5_http_response *response, const char *tag)
{
    char opening[64], closing[64];
    int open_size = snprintf(opening, sizeof(opening), "<%s>", tag);
    int close_size = snprintf(closing, sizeof(closing), "</%s>", tag);
    if (open_size <= 0 || close_size <= 0 || (size_t)open_size >= sizeof(opening) ||
        (size_t)close_size >= sizeof(closing))
        return (sp5_bytes){0};
    sp5_buffer fields = response->fields;
    for (size_t i = 0; i + (size_t)open_size <= fields.size; ++i) {
        if (memcmp(fields.data + i, opening, (size_t)open_size) != 0)
            continue;
        size_t start = i + (size_t)open_size;
        for (size_t j = start; j + (size_t)close_size <= fields.size; ++j)
            if (memcmp(fields.data + j, closing, (size_t)close_size) == 0)
                return (sp5_bytes){fields.data + start, j - start};
    }
    return (sp5_bytes){0};
}

static bool paired_flag(const sp5_http_response *response)
{
    sp5_bytes value = field(response, "paired");
    return value.size == 1 && value.data[0] == '1';
}

static int nibble(uint8_t digit)
{
    if (digit >= '0' && digit <= '9')
        return digit - '0';
    if (digit >= 'a' && digit <= 'f')
        return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F')
        return digit - 'A' + 10;
    return -1;
}

static bool unhex(sp5_bytes value, uint8_t *output, size_t capacity, size_t *size)
{
    if (value.size % 2 != 0 || value.size / 2 > capacity)
        return false;
    for (size_t i = 0; i < value.size / 2; ++i) {
        int high = nibble(value.data[2 * i]), low = nibble(value.data[2 * i + 1]);
        if (high < 0 || low < 0)
            return false;
        output[i] = (uint8_t)((high << 4) | low);
    }
    *size = value.size / 2;
    return true;
}

static bool aes(const uint8_t key[16], sp5_bytes input, uint8_t *output, bool encrypt)
{
    if (input.size == 0 || input.size > 48 || input.size % 16 != 0)
        return false;
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    int written = 0, final = 0;
    bool valid =
        context != NULL &&
        EVP_CipherInit_ex(context, EVP_aes_128_ecb(), NULL, key, NULL, encrypt ? 1 : 0) == 1 &&
        EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
        EVP_CipherUpdate(context, output, &written, input.data, (int)input.size) == 1 &&
        EVP_CipherFinal_ex(context, output + written, &final) == 1 &&
        (size_t)written + (size_t) final == input.size;
    EVP_CIPHER_CTX_free(context);
    return valid;
}

static bool challenge_digest(X509 *certificate, const uint8_t challenge[16],
                             const uint8_t secret[16], uint8_t out[32])
{
    const ASN1_BIT_STRING *signature = NULL;
    X509_get0_signature(&signature, NULL, certificate);
    if (signature == NULL || signature->length <= 0 || signature->length > RSA_LIMIT)
        return false;
    uint8_t input[RSA_LIMIT + 2 * SECRET_SIZE];
    memcpy(input, challenge, SECRET_SIZE);
    memcpy(input + SECRET_SIZE, signature->data, (size_t)signature->length);
    memcpy(input + SECRET_SIZE + (size_t)signature->length, secret, SECRET_SIZE);
    size_t size = 0;
    return EVP_Q_digest(NULL, "SHA256", NULL, input,
                        (size_t)signature->length + (size_t)2 * SECRET_SIZE, out, &size) == 1 &&
           size == 32;
}

static sp5_result exchange(sp5_pair_flow *flow, const char *key, sp5_bytes value, uint64_t now_us,
                           sp5_optional_u64 wall_us, sp5_http_response *response)
{
    char target[TARGET_LIMIT];
    size_t size = 0;
    if (!append_text(target, sizeof(target), &size, "/pair?uniqueid=") ||
        !append_text(target, sizeof(target), &size, unique_id) ||
        !append_text(target, sizeof(target), &size, "&") ||
        !append_text(target, sizeof(target), &size, key) ||
        !append_text(target, sizeof(target), &size, "=") ||
        !append_hex(target, sizeof(target), &size, value))
        return sp5_failure(SP5_REASON_CAPACITY);
    sp5_request request = {.method = {(const uint8_t *)"GET", 3},
                           .target = {(const uint8_t *)target, size}};
    response->fields.size = 0;
    response->code = 200;
    response->deferred = false;
    return sp5_pair_flow_handle(flow, &request, (sp5_bytes){0}, now_us, wall_us, response);
}

sp5_result sp5_fake_pair_client_begin(sp5_fake_pair_client *client, sp5_pair_flow *flow,
                                      sp5_pairing *state, sp5_bytes client_name, uint64_t now_us,
                                      unsigned *code, bool *deferred)
{
    if (client == NULL || client->certificate == NULL || flow == NULL || state == NULL ||
        code == NULL || deferred == NULL || (client_name.data == NULL && client_name.size != 0))
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (RAND_bytes(client->salt, sizeof(client->salt)) != 1)
        return sp5_failure(SP5_REASON_PLATFORM);
    char target[TARGET_LIMIT];
    size_t size = 0;
    if (!append_text(target, sizeof(target), &size, "/pair?uniqueid=") ||
        !append_text(target, sizeof(target), &size, unique_id) ||
        !append_text(target, sizeof(target), &size, "&devicename=") ||
        !append_percent(target, sizeof(target), &size, client_name) ||
        !append_text(target, sizeof(target), &size, "&phrase=getservercert&salt=") ||
        !append_hex(target, sizeof(target), &size, (sp5_bytes){client->salt, 16}) ||
        !append_text(target, sizeof(target), &size, "&clientcert=") ||
        !append_hex(target, sizeof(target), &size,
                    (sp5_bytes){client->certificate_pem, client->certificate_size}))
        return sp5_failure(SP5_REASON_CAPACITY);
    sp5_request request = {.method = {(const uint8_t *)"GET", 3},
                           .target = {(const uint8_t *)target, size}};
    uint8_t fields[SP5_HTTP_LIMIT];
    sp5_http_response response = {.code = 200, .fields = {fields, sizeof(fields), 0}};
    sp5_result result = sp5_pair_flow_handle(flow, &request, (sp5_bytes){0}, now_us,
                                             (sp5_optional_u64){0}, &response);
    if (result.status != SP5_OK)
        return result;
    *code = response.code;
    *deferred = response.deferred;
    client->request_id = response.deferred ? sp5_pairing_read(state, now_us).request_id : 0;
    return sp5_success();
}

sp5_result sp5_fake_pair_client_prove(sp5_fake_pair_client *client, sp5_pair_flow *flow,
                                      sp5_bytes pin, uint64_t now_us, sp5_optional_u64 wall_us,
                                      bool *paired)
{
    if (client == NULL || client->key == NULL || flow == NULL || paired == NULL ||
        client->request_id == 0)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    *paired = false;
    uint8_t fields[SP5_HTTP_LIMIT];
    sp5_http_response response = {.code = 200, .fields = {fields, sizeof(fields), 0}};
    sp5_result result = sp5_pair_flow_take_certificate(flow, client->request_id, now_us, &response);
    if (result.status != SP5_OK)
        return result;
    if (!paired_flag(&response)) {
        client->request_id = 0;
        return sp5_success();
    }
    uint8_t key[16], challenge[SECRET_SIZE], secret[SECRET_SIZE], encrypted[48], reply[64],
        plain[48], digest[32], proof[SECRET_SIZE + RSA_LIMIT];
    sp5_buffer key_buffer = {key, sizeof(key), 0};
    size_t reply_size = 0, signature_size = RSA_LIMIT;
    result = sp5_pairing_pin_key((sp5_bytes){client->salt, 16}, pin, &key_buffer);
    bool valid = result.status == SP5_OK && RAND_bytes(challenge, sizeof(challenge)) == 1 &&
                 RAND_bytes(secret, sizeof(secret)) == 1 &&
                 aes(key, (sp5_bytes){challenge, sizeof(challenge)}, encrypted, true);
    if (valid)
        result = exchange(flow, "clientchallenge", (sp5_bytes){encrypted, 16}, now_us, wall_us,
                          &response);
    valid = valid && result.status == SP5_OK && paired_flag(&response) &&
            unhex(field(&response, "challengeresponse"), reply, sizeof(reply), &reply_size) &&
            reply_size == 48 && aes(key, (sp5_bytes){reply, 48}, plain, false) &&
            challenge_digest(client->certificate, plain + 32, secret, digest) &&
            aes(key, (sp5_bytes){digest, 32}, encrypted, true);
    if (valid)
        result = exchange(flow, "serverchallengeresp", (sp5_bytes){encrypted, 32}, now_us, wall_us,
                          &response);
    EVP_MD_CTX *sign =
        valid && result.status == SP5_OK && paired_flag(&response) ? EVP_MD_CTX_new() : NULL;
    memcpy(proof, secret, SECRET_SIZE);
    valid = sign != NULL && EVP_DigestSignInit(sign, NULL, EVP_sha256(), NULL, client->key) == 1 &&
            EVP_DigestSign(sign, proof + SECRET_SIZE, &signature_size, secret, SECRET_SIZE) == 1;
    EVP_MD_CTX_free(sign);
    if (valid)
        result =
            exchange(flow, "clientpairingsecret", (sp5_bytes){proof, SECRET_SIZE + signature_size},
                     now_us, wall_us, &response);
    if (result.status == SP5_OK)
        *paired = valid && paired_flag(&response);
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(secret, sizeof(secret));
    OPENSSL_cleanse(plain, sizeof(plain));
    OPENSSL_cleanse(proof, sizeof(proof));
    client->request_id = 0;
    return result;
}
