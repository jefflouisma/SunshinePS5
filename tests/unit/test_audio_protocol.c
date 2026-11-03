#include "protocol/audio.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (fputs(__FILE__ ": " #c "\n", stderr) == EOF)                                       \
                return 2;                                                                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static size_t allocations;
static void *allocate(size_t n, const char *f, int l)
{
    (void)f;
    (void)l;
    ++allocations;
    return malloc(n);
}
static void *reallocate(void *p, size_t n, const char *f, int l)
{
    (void)f;
    (void)l;
    ++allocations;
    return realloc(p, n);
}
static void release(void *p, const char *f, int l)
{
    (void)f;
    (void)l;
    free(p);
}
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
/* Independent test-only GF arithmetic, polynomial 0x11d. Simulated erasures
 * verify the host's actual parity bytes, without adding a decoder to the host. */
static uint8_t gf_multiply(uint8_t a, uint8_t b)
{
    unsigned product = 0;
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned j = 0; j < 8; ++j)
            if ((a & (1U << i)) != 0 && (b & (1U << j)) != 0)
                product ^= 1U << (i + j);
    for (unsigned degree = 14; degree >= 8; --degree)
        if ((product & (1U << degree)) != 0)
            product ^= 0x11dU << (degree - 8);
    return (uint8_t)product;
}
static uint8_t gf_inverse(uint8_t value)
{
    for (unsigned candidate = 1; candidate <= 255; ++candidate)
        if (gf_multiply(value, (uint8_t)candidate) == 1)
            return (uint8_t)candidate;
    return 0;
}
static int sequence(void)
{
    /* notes §8, Sunshine d0b4a88 stream.cpp:1871-1958; mcc 41cedb8
     * RtpAudioQueue.h:11-24. FEC RTP indices overlap the next data group. */
    sp5_audio_config c = {.sample_rate = 48000,
                          .channels = 2,
                          .duration_ms = 5,
                          .opus_size = 16,
                          .first_sequence = 65532,
                          .first_timestamp_ms = UINT32_MAX - 9};
    sp5_audio *a = sp5_audio_create(&c);
    CHECK(a != NULL);
    uint8_t payload[16], bytes[SP5_AUDIO_WIRE_LIMIT], groups[2][16], recorded[4][16];
    sp5_buffer wire = {bytes, sizeof(bytes), 0};
    CHECK(sp5_audio_next(a, &wire).reason == SP5_REASON_WOULD_BLOCK);
    for (unsigned i = 0; i < 8; ++i) {
        memset(payload, (int)(i % 4 + 1), sizeof(payload));
        CHECK(sp5_audio_submit(a, (sp5_bytes){payload, 15}).status == SP5_ERROR);
        size_t before = allocations;
        CHECK(sp5_audio_submit(a, (sp5_bytes){payload, sizeof(payload)}).status == SP5_OK);
        CHECK(sp5_audio_submit(a, (sp5_bytes){payload, sizeof(payload)}).reason ==
              SP5_REASON_WOULD_BLOCK);
        wire.capacity = 1;
        CHECK(sp5_audio_next(a, &wire).reason == SP5_REASON_CAPACITY && wire.size == 0);
        wire.capacity = sizeof(bytes);
        CHECK(sp5_audio_next(a, &wire).status == SP5_OK && wire.size == 28);
        CHECK(allocations == before);
        uint16_t seq = (uint16_t)(65532 + i);
        CHECK(bytes[0] == 0x80 && bytes[1] == 97 && bytes[2] == (uint8_t)(seq >> 8) &&
              bytes[3] == (uint8_t)seq);
        CHECK(be32(bytes + 4) == c.first_timestamp_ms + 5U * i && be32(bytes + 8) == 0);
        CHECK(memcmp(bytes + 12, payload, sizeof(payload)) == 0);
        memcpy(recorded[i % 4], bytes + 12, 16);
        if (i % 4 == 3) {
            for (unsigned j = 0; j < 2; ++j) {
                CHECK(sp5_audio_next(a, &wire).status == SP5_OK && wire.size == 40);
                CHECK(bytes[1] == 127 && bytes[12] == j && bytes[13] == 97);
                uint16_t parity_seq = (uint16_t)(seq + 1 + j), base = (uint16_t)(seq - 3);
                CHECK(bytes[2] == (uint8_t)(parity_seq >> 8) && bytes[3] == (uint8_t)parity_seq);
                CHECK(be32(bytes + 4) == 0 && bytes[14] == (uint8_t)(base >> 8) &&
                      bytes[15] == (uint8_t)base);
                CHECK(be32(bytes + 16) == c.first_timestamp_ms + 5U * (i - 3) &&
                      be32(bytes + 20) == 0);
                /* Reference fixed audio matrix {77,40,38,0e;c7,a7,0d,6c},
                 * independent GF(256) arithmetic for constant shards 1,2,3,4. */
                memcpy(groups[j], bytes + 24, 16);
            }
            CHECK(allocations == before);
            /* Erase each possible pair of data shards from the received group.
             * Solve the two parity equations using only the other data shards;
             * do not read either erased vector until comparing reconstruction. */
            const uint8_t matrix[2][4] = {{0x77, 0x40, 0x38, 0x0e}, {0xc7, 0xa7, 0x0d, 0x6c}};
            for (size_t lost_a = 0; lost_a < 4; ++lost_a)
                for (size_t lost_b = lost_a + 1; lost_b < 4; ++lost_b) {
                    uint8_t a0 = matrix[0][lost_a], b0 = matrix[0][lost_b];
                    uint8_t a1 = matrix[1][lost_a], b1 = matrix[1][lost_b];
                    uint8_t determinant = gf_inverse(gf_multiply(a0, b1) ^ gf_multiply(a1, b0));
                    CHECK(determinant != 0);
                    for (size_t byte = 0; byte < 16; ++byte) {
                        uint8_t rhs[2] = {groups[0][byte], groups[1][byte]};
                        for (size_t column = 0; column < 4; ++column)
                            if (column != lost_a && column != lost_b)
                                for (size_t row = 0; row < 2; ++row)
                                    rhs[row] ^=
                                        gf_multiply(matrix[row][column], recorded[column][byte]);
                        uint8_t recovered_a = gf_multiply(
                            gf_multiply(b1, rhs[0]) ^ gf_multiply(b0, rhs[1]), determinant);
                        uint8_t recovered_b = gf_multiply(
                            gf_multiply(a1, rhs[0]) ^ gf_multiply(a0, rhs[1]), determinant);
                        CHECK(recovered_a == recorded[lost_a][byte]);
                        CHECK(recovered_b == recorded[lost_b][byte]);
                    }
                }
        }
        CHECK(sp5_audio_next(a, &wire).reason == SP5_REASON_WOULD_BLOCK);
    }
    /* Fixed byte oracles from the documented audio matrix (not video Cauchy). */
    for (size_t i = 0; i < 16; ++i) {
        CHECK(groups[0][i] == 0x87);
        CHECK(groups[1][i] == 0x2e);
    }
    sp5_audio_destroy(a);
    c.channels = 1;
    CHECK(sp5_audio_create(&c) == NULL);
    c.channels = 2;
    c.duration_ms = 20;
    CHECK(sp5_audio_create(&c) == NULL);
    c.duration_ms = 10;
    c.first_sequence = 1;
    CHECK(sp5_audio_create(&c) == NULL);
    c.first_sequence = 0;
    c.opus_size = SP5_AUDIO_PAYLOAD_LIMIT;
    c.encrypted = true;
    CHECK(sp5_audio_create(&c) == NULL);
    return 0;
}
static int encryption(void)
{
    sp5_audio_config c = {.sample_rate = 48000,
                          .channels = 2,
                          .duration_ms = 10,
                          .opus_size = 16,
                          .key_id = UINT32_MAX - 1,
                          .encrypted = true};
    memset(c.key, 0x42, 16);
    sp5_audio *a = sp5_audio_create(&c);
    CHECK(a != NULL);
    uint8_t bytes[SP5_AUDIO_WIRE_LIMIT], data[16], plaintext[64];
    for (size_t i = 0; i < sizeof(data); ++i)
        data[i] = (uint8_t)i;
    sp5_buffer wire = {bytes, sizeof(bytes), 0};
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    CHECK(ctx != NULL);
    for (unsigned i = 0; i < 4; ++i) {
        size_t before = allocations;
        CHECK(sp5_audio_submit(a, (sp5_bytes){data, sizeof(data)}).status == SP5_OK);
        CHECK(sp5_audio_next(a, &wire).status == SP5_OK && wire.size == 44);
        CHECK(allocations == before);
        /* notes §8, mcc 41cedb8 AudioStream.c:178-201. Independent locked
         * Rust cbc 0.1.2 / aes 0.8.4 oracle: key 42x16, IV fffffffe|00x12,
         * plaintext 00..0f plus PKCS#7's full 16-byte padding block. */
        const uint8_t expected_cipher[32] = {0xb8, 0x11, 0x03, 0x69, 0x11, 0xab, 0x12, 0xde,
                                             0x4b, 0x37, 0x35, 0xb5, 0x9f, 0x08, 0xab, 0x73,
                                             0x2f, 0xb3, 0xca, 0xd9, 0x79, 0xaf, 0x87, 0xcc,
                                             0x7d, 0xcd, 0xec, 0xd9, 0x9d, 0x9e, 0xc1, 0xb2};
        if (i == 0)
            CHECK(memcmp(bytes + 12, expected_cipher, sizeof(expected_cipher)) == 0);
        uint8_t iv[16] = {0};
        uint32_t value = c.key_id + i;
        for (unsigned j = 0; j < 4; ++j)
            iv[j] = (uint8_t)(value >> (24 - 8 * j));
        int n = 0, tail = 0;
        CHECK(EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, c.key, iv) == 1);
        CHECK(EVP_DecryptUpdate(ctx, plaintext, &n, bytes + 12, 32) == 1);
        CHECK(EVP_DecryptFinal_ex(ctx, plaintext + n, &tail) == 1);
        CHECK(n + tail == 16 && memcmp(plaintext, data, 16) == 0);
    }
    CHECK(sp5_audio_next(a, &wire).status == SP5_OK && wire.size == 56 && bytes[1] == 127);
    CHECK(sp5_audio_next(a, &wire).status == SP5_OK && bytes[12] == 1);
    CHECK(sp5_audio_next(a, &wire).reason == SP5_REASON_WOULD_BLOCK);
    EVP_CIPHER_CTX_free(ctx);
    sp5_audio_destroy(a);
    return 0;
}
int main(void)
{
    CHECK(CRYPTO_set_mem_functions(allocate, reallocate, release) == 1);
    CHECK(sequence() == 0);
    CHECK(encryption() == 0);
    return 0;
}
