#include "audio.h"
#include "fec.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

/* notes §8, Sunshine d0b4a88 stream.cpp:1871-1958,2371-2387;
 * mcc 41cedb8 RtpAudioQueue.h:11-24, AudioStream.c:178-201.
 * Payloads (ciphertext when encrypted) persist for exactly one 4-data group.
 * Output descriptors are at most one data and two parity packets. */
struct sp5_audio {
    sp5_audio_config config;
    sp5_fec fec;
    EVP_CIPHER_CTX *cipher;
    uint8_t shards[6][SP5_AUDIO_PAYLOAD_LIMIT];
    size_t payload_size, queued, cursor, data_slot;
    uint16_t sequence, output_sequence, base_sequence;
    uint32_t timestamp, output_timestamp, base_timestamp;
    bool failed;
};
static void put16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)(n >> 8);
    p[1] = (uint8_t)n;
}
static void put32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(n >> (24 - 8 * i));
}
sp5_audio *sp5_audio_create(const sp5_audio_config *config)
{
    if (config == NULL || config->sample_rate != 48000 || config->channels != 2 ||
        (config->duration_ms != 5 && config->duration_ms != 10) ||
        config->first_sequence % 4 != 0 || config->opus_size == 0 ||
        config->opus_size > SP5_AUDIO_PAYLOAD_LIMIT)
        return NULL;
    size_t size = config->encrypted ? (config->opus_size / 16U + 1U) * 16U : config->opus_size;
    if (size > SP5_AUDIO_PAYLOAD_LIMIT)
        return NULL;
    sp5_audio *a = calloc(1, sizeof(*a));
    if (a == NULL)
        return NULL;
    a->config = *config;
    a->sequence = config->first_sequence;
    a->timestamp = config->first_timestamp_ms;
    a->payload_size = size;
    sp5_fec_init(&a->fec);
    if (config->encrypted) {
        uint8_t iv[16] = {0};
        a->cipher = EVP_CIPHER_CTX_new();
        if (a->cipher == NULL ||
            EVP_EncryptInit_ex(a->cipher, EVP_aes_128_cbc(), NULL, config->key, iv) != 1) {
            sp5_audio_destroy(a);
            return NULL;
        }
    }
    return a;
}
void sp5_audio_destroy(sp5_audio *a)
{
    if (a == NULL)
        return;
    EVP_CIPHER_CTX_free(a->cipher);
    OPENSSL_cleanse(a, sizeof(*a));
    free(a);
}
sp5_result sp5_audio_submit(sp5_audio *a, sp5_bytes opus)
{
    if (a == NULL || opus.data == NULL || opus.size != a->config.opus_size)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (a->failed)
        return sp5_failure(SP5_REASON_PLATFORM);
    if (a->queued != 0)
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    a->data_slot = a->sequence % 4U;
    uint8_t *out = a->shards[a->data_slot];
    if (a->config.encrypted) {
        uint8_t iv[16] = {0};
        put32(iv, a->config.key_id + (uint32_t)a->sequence);
        int n = 0, tail = 0;
        if (EVP_EncryptInit_ex(a->cipher, NULL, NULL, NULL, iv) != 1 ||
            EVP_EncryptUpdate(a->cipher, out, &n, opus.data, (int)opus.size) != 1 ||
            EVP_EncryptFinal_ex(a->cipher, out + n, &tail) != 1 ||
            (size_t)n + (size_t)tail != a->payload_size) {
            a->failed = true;
            return sp5_failure(SP5_REASON_PLATFORM);
        }
    } else {
        memcpy(out, opus.data, opus.size);
    }
    if (a->data_slot == 0) {
        a->base_sequence = a->sequence;
        a->base_timestamp = a->timestamp;
    }
    if (a->data_slot == 3) {
        uint8_t *shards[6];
        for (size_t i = 0; i < 6; ++i)
            shards[i] = a->shards[i];
        sp5_result result = sp5_fec_audio(&a->fec, shards, a->payload_size);
        if (result.status != SP5_OK) {
            a->failed = true;
            return result;
        }
    }
    a->output_sequence = a->sequence++;
    a->output_timestamp = a->timestamp;
    a->timestamp += a->config.duration_ms;
    a->queued = a->data_slot == 3 ? 3 : 1;
    a->cursor = 0;
    return sp5_success();
}
sp5_result sp5_audio_next(sp5_audio *a, sp5_buffer *wire)
{
    if (wire != NULL)
        wire->size = 0;
    if (a == NULL || wire == NULL || wire->data == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (a->failed)
        return sp5_failure(SP5_REASON_PLATFORM);
    if (a->queued == 0)
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    bool parity = a->cursor != 0;
    size_t header = parity ? 24U : 12U;
    if (wire->capacity < header + a->payload_size)
        return sp5_failure(SP5_REASON_CAPACITY);
    uint8_t *p = wire->data;
    memset(p, 0, header);
    p[0] = 0x80;
    p[1] = parity ? 127 : 97;
    put16(p + 2, (uint16_t)(a->output_sequence + a->cursor));
    if (parity) {
        p[12] = (uint8_t)(a->cursor - 1U);
        p[13] = 97;
        put16(p + 14, a->base_sequence);
        put32(p + 16, a->base_timestamp);
    } else {
        put32(p + 4, a->output_timestamp);
    }
    size_t slot = parity ? 3U + a->cursor : a->data_slot;
    memcpy(p + header, a->shards[slot], a->payload_size);
    wire->size = header + a->payload_size;
    ++a->cursor;
    --a->queued;
    return sp5_success();
}
