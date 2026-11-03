#include "video.h"
#include "fec.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

/* Packet/frame layout, block flags and crypto: protocol-notes §§5-7,
 * Sunshine d0b4a88 stream.cpp:1588-1779; mcc 41cedb8 Video.h:12-48.
 * The fixed pool is one FEC block, reused only after all its shards leave. */
struct sp5_video {
    sp5_video_config config;
    sp5_fec fec;
    EVP_CIPHER_CTX *cipher;
    uint8_t pool[SP5_FEC_SHARDS][SP5_VIDEO_PACKET_LIMIT + 16];
    sp5_video_frame_input frame;
    uint8_t header[8];
    size_t data_total, data_done, block_data, block_parity, cursor, blocks, block;
    uint32_t packet_index;
    uint64_t nonce;
    bool active, prepared;
};
static void put_le32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(n >> (8 * i));
}
static void put_le16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}
static void put_be32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(n >> (24 - 8 * i));
}
static size_t max_data(const sp5_video *v)
{
    size_t d = 25500U / (100U + v->config.fec_percent);
    if (v->config.fec_percent != 0 && d > 255U - v->config.minimum_parity)
        d = 255U - v->config.minimum_parity;
    return d;
}
sp5_video *sp5_video_create(const sp5_video_config *config)
{
    if (config == NULL || config->packet_size < 64 ||
        config->packet_size > SP5_VIDEO_PACKET_LIMIT || config->fec_percent > 100 ||
        config->minimum_parity > 2)
        return NULL;
    sp5_video *v = calloc(1, sizeof(*v));
    if (v == NULL)
        return NULL;
    v->config = *config;
    v->packet_index = config->first_packet_index;
    sp5_fec_init(&v->fec);
    if (config->encrypted) {
        uint8_t iv[12] = {0};
        iv[11] = 'V';
        v->cipher = EVP_CIPHER_CTX_new();
        if (v->cipher == NULL ||
            EVP_EncryptInit_ex(v->cipher, EVP_aes_128_gcm(), NULL, config->key, iv) != 1) {
            sp5_video_destroy(v);
            return NULL;
        }
    }
    return v;
}
void sp5_video_destroy(sp5_video *v)
{
    if (v == NULL)
        return;
    EVP_CIPHER_CTX_free(v->cipher);
    OPENSSL_cleanse(v, sizeof(*v));
    free(v);
}
uint16_t sp5_video_latency(sp5_optional_u64 elapsed_us, bool repeated)
{
    if (!elapsed_us.present || repeated)
        return 0;
    uint64_t ticks = elapsed_us.value / 100U + (elapsed_us.value % 100U >= 50U ? 1U : 0U);
    return ticks >= UINT16_MAX ? UINT16_MAX : (uint16_t)ticks;
}
void sp5_video_cancel(sp5_video *v)
{
    if (v == NULL)
        return;
    v->active = false;
    v->prepared = false;
    v->frame = (sp5_video_frame_input){0};
}
sp5_result sp5_video_begin(sp5_video *v, const sp5_video_frame_input *frame)
{
    if (v == NULL || frame == NULL || frame->bitstream.data == NULL || frame->bitstream.size == 0)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (v->active)
        return sp5_failure(SP5_REASON_OWNERSHIP);
    size_t payload = v->config.packet_size - 16U;
    if (frame->bitstream.size > 4U * max_data(v) * payload - 8U)
        return sp5_failure(SP5_REASON_CAPACITY);
    v->frame = *frame;
    v->data_total = (frame->bitstream.size + 8U + payload - 1U) / payload;
    v->data_done = 0;
    v->blocks = (v->data_total + max_data(v) - 1U) / max_data(v);
    v->block = 0;
    memset(v->header, 0, sizeof(v->header));
    v->header[0] = 1;
    put_le16(v->header + 1, sp5_video_latency(frame->processing_us, frame->repeated));
    v->header[3] = frame->key_frame ? 2 : (frame->after_invalidation ? 5 : 1);
    size_t last = (frame->bitstream.size + 8U) % payload;
    put_le16(v->header + 4, (uint16_t)(last == 0 ? payload : last));
    v->active = true;
    v->prepared = false;
    return sp5_success();
}
static sp5_result prepare(sp5_video *v)
{
    size_t payload = v->config.packet_size - 16U, shard_size = v->config.packet_size + 16U;
    size_t remaining = v->data_total - v->data_done;
    v->block_data = remaining < max_data(v) ? remaining : max_data(v);
    unsigned percent = v->config.fec_percent;
    v->block_parity = (v->block_data * percent + 99U) / 100U;
    if (percent != 0 && v->block_parity < v->config.minimum_parity) {
        v->block_parity = v->config.minimum_parity;
        percent = (unsigned)(100U * v->block_parity / v->block_data);
    }
    uint8_t *shards[SP5_FEC_SHARDS];
    for (size_t i = 0; i < v->block_data + v->block_parity; ++i) {
        shards[i] = v->pool[i];
        memset(shards[i], 0, shard_size);
        if (i >= v->block_data)
            continue;
        uint8_t *p = shards[i];
        put_le32(p + 16, (v->packet_index + (uint32_t)i) << 8);
        put_le32(p + 20, v->frame.frame_index);
        p[24] = (uint8_t)(1U | (i == 0 ? 4U : 0U) | (i + 1 == v->block_data ? 2U : 0U));
        p[26] = 0x10;
        p[27] = (uint8_t)((v->block << 4) | ((v->blocks - 1U) << 6));
        size_t offset = (v->data_done + i) * payload;
        size_t available = v->frame.bitstream.size + 8U;
        for (size_t j = 0; j < payload && offset + j < available; ++j)
            p[32 + j] =
                offset + j < 8U ? v->header[offset + j] : v->frame.bitstream.data[offset + j - 8U];
    }
    sp5_result result = sp5_fec_encode(&v->fec, shards, v->block_data, v->block_parity, shard_size);
    if (result.status != SP5_OK)
        return result;
    /* Header columns explicitly rewritten after RS encoding, matching the
     * reference receiver's reconstruction policy; parity flags remain arbitrary. */
    for (size_t i = 0; i < v->block_data + v->block_parity; ++i) {
        uint8_t *p = shards[i];
        uint32_t sequence = v->packet_index + (uint32_t)i;
        p[0] = 0x90;
        p[2] = (uint8_t)(sequence >> 8);
        p[3] = (uint8_t)sequence;
        put_be32(p + 4, v->frame.rtp_timestamp);
        put_le32(p + 20, v->frame.frame_index);
        p[27] = (uint8_t)((v->block << 4) | ((v->blocks - 1U) << 6));
        put_le32(p + 28, (uint32_t)((v->block_data << 22) | (i << 12) | (percent << 4)));
    }
    v->cursor = 0;
    v->prepared = true;
    return sp5_success();
}
sp5_result sp5_video_next(sp5_video *v, sp5_buffer *wire)
{
    if (wire != NULL)
        wire->size = 0;
    if (v == NULL || wire == NULL || wire->data == NULL)
        return sp5_failure(SP5_REASON_INVALID_ARGUMENT);
    if (!v->active)
        return sp5_failure(SP5_REASON_WOULD_BLOCK);
    size_t size = v->config.packet_size + 16U;
    if (wire->capacity < size + (v->config.encrypted ? 32U : 0U))
        return sp5_failure(SP5_REASON_CAPACITY);
    if (!v->prepared) {
        sp5_result result = prepare(v);
        if (result.status != SP5_OK) {
            sp5_video_cancel(v);
            return result;
        }
    }
    uint8_t *p = v->pool[v->cursor];
    if (v->config.encrypted) {
        if (v->nonce == UINT64_MAX) {
            sp5_video_cancel(v);
            return sp5_failure(SP5_REASON_OVERFLOW);
        }
        memset(wire->data, 0, 32);
        uint64_t nonce = v->nonce++;
        for (unsigned i = 0; i < 8; ++i)
            wire->data[i] = (uint8_t)(nonce >> (8 * i));
        wire->data[11] = 'V';
        put_le32(wire->data + 12, v->frame.frame_index);
        int n = 0, tail = 0;
        if (EVP_EncryptInit_ex(v->cipher, NULL, NULL, NULL, wire->data) != 1 ||
            EVP_EncryptUpdate(v->cipher, wire->data + 32, &n, p, (int)size) != 1 ||
            EVP_EncryptFinal_ex(v->cipher, wire->data + 32 + n, &tail) != 1 ||
            EVP_CIPHER_CTX_ctrl(v->cipher, EVP_CTRL_GCM_GET_TAG, 16, wire->data + 16) != 1) {
            sp5_video_cancel(v);
            return sp5_failure(SP5_REASON_PLATFORM);
        }
        wire->size = (size_t)n + (size_t)tail + 32U;
    } else {
        memcpy(wire->data, p, size);
        wire->size = size;
    }
    ++v->packet_index;
    if (++v->cursor == v->block_data + v->block_parity) {
        v->data_done += v->block_data;
        v->prepared = false;
        if (++v->block == v->blocks)
            sp5_video_cancel(v);
    }
    return sp5_success();
}
